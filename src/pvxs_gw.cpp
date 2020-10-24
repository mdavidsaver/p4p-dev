
#include "p4p.h"

#include <pvxs/source.h>
#include <pvxs/sharedpv.h>
#include <pvxs/client.h>
#include <pvxs/log.h>

#include "pvxs_gw.h"
#include "_gw.h"

DEFINE_LOGGER(_log, "p4p.gw");

namespace p4p {

void GWSource::onSearch(Search &op)
{
    // on server worker

    Guard G(mutex);

    decltype (banHostPV)::value_type pair;
    pair.first = op.source();

    if(banHost.find(pair.first)!=banHost.end()) {
        log_debug_printf(_log, "%p ignore banned host '%s'\n", this, pair.first.c_str());
        return;
    }

    for(auto& chan : op) {
        pair.second = chan.name();

        if(banPV.find(pair.second)!=banPV.end()) {
            log_debug_printf(_log, "%p ignore banned PV '%s'\n", this, pair.second.c_str());
            continue;
        } else if(banHost.find(pair.first)!=banHost.end()) {
            log_debug_printf(_log, "%p ignore banned Host '%s'\n", this, pair.first.c_str());
            continue;
        } else if(banHostPV.find(pair)!=banHostPV.end()) {
            log_debug_printf(_log, "%p ignore banned Host+PV '%s':'%s'\n", this, pair.first.c_str(), pair.second.c_str());
            continue;
        }

        GWSearchResult result = GWSearchIgnore;
        {
            // GWProvider_testChannel() will also lock our mutex, but must unlock first
            // to maintain lock order ordering wrt. GIL.
            UnGuard U(G);
            PyLock L;

            result = (GWSearchResult)GWProvider_testChannel(handler, chan.name(), op.source());
        }
        log_debug_printf(_log, "%p testChannel '%s':'%s' -> %d\n", this, pair.first.c_str(), pair.second.c_str(), result);

        switch(result) {
        case GWSearchClaim:
            chan.claim();
            break;
        case GWSearchBanHost:
            banHost.insert(pair.first);
            break;
        case GWSearchBanPV:
            banPV.insert(pair.second);
            break;
        case GWSearchBanHostPV:
            banHostPV.insert(pair);
            break;
        case GWSearchIgnore:
            break;
        }
    }
}

void GWSource::onCreate(std::unique_ptr<server::ChannelControl> &&op)
{
    // on server worker

    // Server worker may make synchronous calls to client worker.
    // To avoid deadlock, client worker must not make synchronous calls to server worker

    // Server operation handles may hold strong references to client operation handles
    // To avoid a reference loop, client operation handles must not hold strong refs.
    // to server handles.

    std::shared_ptr<server::ChannelControl> ctrl(std::move(op));

    std::shared_ptr<GWChan> pv;
    {
        PyLock L;

        pv = GWProvider_makeChannel(this, ctrl);
    }

    if(!pv || !pv->us->connector->connected()) {
        log_debug_printf(_log, "%p makeChannel returned %s '%s'\n", this, pv ? "disconnected" : "null", ctrl->name().c_str());
        ctrl->close();
        return;
    }

    ctrl->onRPC([pv, this](std::unique_ptr<server::ExecOp>&& op, Value&& arg) mutable {
        // on server worker

        std::shared_ptr<server::ExecOp> sop(std::move(op));

        bool permit = pv->allow_put;

        log_debug_printf(_log, "%p '%s' RPC %s\n", this, sop->name().c_str(), permit ? "begin" : "DENY");

        if(!permit) {
            op->error("RPC permission denied by gateway");
            return;
        }

        auto cliop = pv->us->upstream.rpc(pv->us->usname, arg)
                .syncCancel(false)
                .result([sop, this](client::Result&& result)
        {
            // on client worker

            log_debug_printf(_log, "%p '%s' RPC complete\n", this, sop->name().c_str());

            // syncs client worker with server worker
            try {
                sop->reply(result());
            }catch(client::RemoteError& e) {
                sop->error(e.what());
            }catch(std::exception& e) {
                log_err_printf(_log, "RPC error: %s\n", e.what());
                sop->error(std::string("Error: ")+e.what());
            }
        })
                .exec();

        // just need to keep client op alive
        sop->onCancel([cliop]() {});
    });

    ctrl->onOp([pv, this](std::unique_ptr<server::ConnectOp>&& sop) mutable { // INFO/GET/PUT
        // on server worker
        // 1. downstream creating operation

        std::shared_ptr<server::ConnectOp> ctrl(std::move(sop));

        if(ctrl->op()==server::ConnectOp::Info) {
            log_debug_printf(_log, "%p '%s' INFO\n", this, ctrl->name().c_str()); // ============ INFO

            auto cliop = pv->us->upstream.info(pv->us->usname)
                    .syncCancel(false)
                    .result([ctrl, this](client::Result&& result)
            {
                // on client worker

                log_debug_printf(_log, "%p '%s' GET INFO done\n", this, ctrl->name().c_str());

                try{
                    ctrl->connect(result());
                }catch(std::exception& e){
                    ctrl->error(e.what());
                    return;
                }
            })
                    .exec();

            // just need to keep client op alive
            ctrl->onClose([cliop](const std::string&) {});

        } else if(ctrl->op()==server::ConnectOp::Get || ctrl->op()==server::ConnectOp::Put) {
            log_debug_printf(_log, "%p '%s' GET/PUT init\n", this, ctrl->name().c_str()); // ============ GET/PUT

            auto result = [ctrl, this](client::Result&& result)
            {
                // on client worker
                // 2. error prior to reExec()

                // syncs client worker with server worker
                try {
                    result();
                    ctrl->error("onInit() unexpected success/error");
                    log_err_printf(_log, "onInit() unexpected success/error%s", "!");
                } catch (std::exception& e) {
                    ctrl->error(e.what());
                    log_debug_printf(_log, "%p '%s' GET init error: %s\n", this, ctrl->name().c_str(), e.what());
                }
            };

            auto onInit = [ctrl, this](const Value& prototype)
            {
                // on client worker
                // 2. upstream connected and (proto)type definition is available

                log_debug_printf(_log, "%p '%s' GET typed\n", this, ctrl->name().c_str());

                // syncs client worker with server worker
                ctrl->connect(prototype);
                // downstream may now execute
            };

            std::shared_ptr<client::Operation> cliop;
            bool docache;

            // 1. Initiate operation
            if(ctrl->op()==server::ConnectOp::Get) {
                auto pvReq(ctrl->pvRequest());
                docache = true;
                pvReq["record._options.cache"].as<bool>(docache);

                if(!docache && !pv->allow_uncached) {
                    ctrl->error("Gateway disallows uncachable get");
                    return;
                }

                auto builder(pv->us->upstream.get(pv->us->usname)
                             .autoExec(false)
                             .syncCancel(false)
                             .result(std::move(result))
                             .onInit(std::move(onInit)));

                if(!docache)
                    builder.rawRequest(ctrl->pvRequest());

                cliop = builder.exec();

            } else { // Put
                docache = false;

                cliop = pv->us->upstream.put(pv->us->usname)
                    .autoExec(false)
                    .syncCancel(false)
                    .rawRequest(ctrl->pvRequest()) // for PUT, always pass through w/o cache/dedup
                    .result(std::move(result))
                    .onInit(std::move(onInit))
                    .exec();
            }

            // handles both plain CMD_GET as well as Get action on CMD_PUT
            ctrl->onGet([cliop, this](std::unique_ptr<server::ExecOp>&& sop){
                // on server worker
                // 3. downstream executes
                std::shared_ptr<server::ExecOp> op(std::move(sop));

                log_debug_printf(_log, "%p '%s' GET exec\n", this, op->name().c_str());

                if(op->op()==server::ConnectOp::Get) {
                    // TODO: delay when rate limited

                } else {
                    // never caching/limiting Get on CMD_PUT
                    // hopefully no one notices this loophole in policy...
                }

                // async request from server to client
                cliop->reExecGet([op, this](client::Result&& result) {
                    // on client worker
                    // 4. upstream execution complete

                    log_debug_printf(_log, "%p '%s' GET exec done\n", this, op->name().c_str());

                    // syncs client worker with server worker
                    try {
                        op->reply(result());
                    } catch (std::exception& e) {
                        op->error(e.what());
                    }
                });
            });

            ctrl->onPut([cliop, pv, this](std::unique_ptr<server::ExecOp>&& sop, Value&& arg){
                // on server worker
                // 3. downstream executes
                std::shared_ptr<server::ExecOp> op(std::move(sop));

                bool permit = pv->allow_put;
                // TODO: audit

                log_debug_printf(_log, "%p '%s' PUT exec%s\n", this, op->name().c_str(), permit ? "" : " DENY");

                if(!permit) {
                    op->error("Put permission denied by gateway");
                    return;
                }

                // async request from server to client
                cliop->reExecPut(arg, [op, this](client::Result&& result) {
                    // on client worker
                    // 4. upstream execution complete

                    log_debug_printf(_log, "%p '%s' PUT exec done\n", this, op->name().c_str());

                    // syncs client worker with server worker
                    try {
                        result();
                        op->reply();
                    } catch (std::exception& e) {
                        op->error(e.what());
                    }
                });
            });

            // just need to keep client op alive
            ctrl->onClose([cliop](const std::string&) {});

        } else {
            ctrl->error(SB()<<"p4p.gw unsupported operation "<<ctrl->op());
        }
    }); // onOp

    ctrl->onSubscribe([pv, this](std::unique_ptr<server::MonitorSetupOp>&& sop) mutable {
        // on server worker

        std::shared_ptr<server::MonitorSetupOp> op(std::move(sop));

        log_debug_printf(_log, "%p '%s' MONITOR init\n", this, op->name().c_str());

        auto cli = pv->us->upstream.monitor(pv->us->usname)
                .autoExec(false)
                .syncCancel(false)
                .maskConnected(true) // upstream should already be connected
                .maskDisconnected(true) // handled by the client Connect op
                .event([op, this](client::Subscription& cli)
        {
            // on client worker

            // only invoked if there is an early error.
            // replaced below for starting

            try {
                cli.pop(); // expected to throw
                log_warn_printf(_log, "%p '%s' MONITOR setup error??\n", this, op->name().c_str());
            }catch(std::exception& e){
                log_warn_printf(_log, "%p '%s' MONITOR setup error %s\n", this, op->name().c_str(), e.what());
            }
        })
                .onInit([op, this](client::Subscription& cli, const Value& prototype)
        {
            // on client worker

            log_debug_printf(_log, "%p '%s' MONITOR typed\n", this, op->name().c_str());

            auto clisub(cli.shared_from_this());

            // syncs client worker with server worker
            std::shared_ptr<server::MonitorControlOp> srv(op->connect(prototype));

            // syncs client worker with server worker
            srv->onStart([clisub, this](bool s) {
                // on server worker

                log_debug_printf(_log, "%p '%s' MONITOR %s\n", this, clisub->name().c_str(), s?"start":"stop");

                clisub->pause(!s);
            });

            cli.onEvent([srv, this](client::Subscription& cli) {
                    // on client worker

                    log_debug_printf(_log, "%p '%s' MONITOR wakeup\n", this, srv->name().c_str());

                    while(true) {
                        try {
                            auto val(cli.pop());
                            if(!val)
                                break;
                            srv->forcePost(val);
                            log_debug_printf(_log, "%p '%s' MONITOR event\n", this, srv->name().c_str());
                         } catch(client::Finished&) {
                             srv->finish();
                             log_debug_printf(_log, "%p '%s' MONITOR finish\n", this, srv->name().c_str());
                         } catch(std::exception& e) {

                             log_warn_printf(_log, "%p '%s' MONITOR error: %s\n",
                                              this, srv->name().c_str(), e.what());
                         }
                    }
            });
        })
                .exec();

        // just need to keep the client subscription alive
        op->onClose([cli](const std::string&) {});

    }); // onSubscribe
}

GWSearchResult GWSource::test(const std::string &usname)
{
    Guard G(mutex);

    auto it(channels.find(usname));

    log_debug_printf(_log, "%p '%s' channel cache %s\n", this, usname.c_str(),
                     (it==channels.end()) ? "miss" : "hit");
    if(it==channels.end()) {

        auto chan(std::make_shared<GWUpstream>(usname, upstream));

        auto pair = channels.insert(std::make_pair(usname, chan));
        assert(pair.second); // we already checked
        it = pair.first;
    }

    it->second->gcmark = false;
    auto usconn = it->second->connector->connected();

    log_debug_printf(_log, "%p test '%s' -> %c\n", this, usname.c_str(), usconn ? '!' : '_');

    return usconn ? GWSearchClaim : GWSearchIgnore;
}



std::shared_ptr<GWChan> GWSource::connect(const std::string& dsname,
                                          const std::string& usname,
                                          const std::shared_ptr<server::ChannelControl>& op)
{
    std::shared_ptr<GWChan> ret;

    Guard G(mutex);

    auto it(channels.find(usname));
    if(it!=channels.end() && it->second->connector->connected()) {
        ret.reset(new GWChan(dsname, it->second));
    }

    log_debug_printf(_log, "%p connect '%s' as '%s' -> %c\n", this, usname.c_str(), dsname.c_str(), ret ? '!' : '_');

    return ret;
}

void GWSource::sweep()
{
    log_debug_printf(_log, "%p sweeps\n", this);

    std::vector<std::shared_ptr<GWUpstream>> trash;
    // garbage disposal after unlock
    Guard G(mutex);

    {
        auto it(channels.begin()), end(channels.end());
        while(it!=end) {
            auto cur(it++);

            if(!it->second->gcmark) {
                it->second->gcmark = true;

            } else if(it->second.use_count()<=1u) { // one for GWSource::channels map
                log_debug_printf(_log, "%p swept '%s'\n", this, it->first.c_str());
                trash.emplace_back(std::move(it->second));
                channels.erase(it);
            }
        }
    }
}

void GWSource::disconnect(const std::string& usname) {}
void GWSource::forceBan(const std::string& host, const std::string& usname) {}
void GWSource::clearBan() {}

void GWSource::cachePeek(std::set<std::string> &names) const {}

} // namespace p4p
