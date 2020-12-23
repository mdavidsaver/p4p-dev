#ifndef PVXS_GW_H
#define PVXS_GW_H

#include <atomic>

#include "p4p.h"

#include <pvxs/source.h>
#include <pvxs/sharedpv.h>
#include <pvxs/client.h>

namespace p4p {
using namespace pvxs;

struct GWChan;

enum GWSearchResult {
    GWSearchIgnore,
    GWSearchClaim,
    GWSearchBanHost,
    GWSearchBanPV,
    GWSearchBanHostPV,
};

struct GWSubscription {
    //const std::shared_ptr<GWUpstream> chan;

    // should only be lock()'d from server worker
    std::weak_ptr<client::Subscription> upstream;

    Value current;

    enum state_t {
        Connecting, // waiting for onInit()
        Running,
        Error,
    } state = Connecting;

    std::vector<std::shared_ptr<server::MonitorSetupOp>> setups;
    std::vector<std::shared_ptr<server::MonitorControlOp>> controls;
};

struct GWUpstream {
    const std::string usname;
    client::Context upstream;

    const std::shared_ptr<client::Connect> connector;

    mutable epicsMutex dschans_lock;
    std::set<std::shared_ptr<server::ChannelControl>> dschans;

    // time in msec
    std::atomic<unsigned> get_holdoff{};

    epicsMutex lock;

    std::weak_ptr<GWSubscription> subscription;

    bool gcmark = false;

    GWUpstream(const std::string& usname, client::Context& ctxt);
};

struct GWChan {
    const std::string dsname;
    const std::shared_ptr<GWUpstream> us;
    const std::shared_ptr<server::ChannelControl> dschannel;

    // Use atomic access.
    // binary flags
    std::atomic<bool> allow_put{},
                      allow_rpc{},
                      allow_uncached{},
                      audit{};


    GWChan(const std::string& dsname,
           const std::shared_ptr<GWUpstream>& upstream,
           const std::shared_ptr<server::ChannelControl>& dschannel);
    ~GWChan();

    static
    void onRPC(const std::shared_ptr<GWChan>& self, std::unique_ptr<server::ExecOp>&& op, Value&& arg);
    static
    void onOp(const std::shared_ptr<GWChan>& self, std::unique_ptr<server::ConnectOp>&& sop);
    static
    void onSubscribe(const std::shared_ptr<GWChan>& self, std::unique_ptr<server::MonitorSetupOp>&& sop);
};

struct GWSource : public server::Source,
                  public std::enable_shared_from_this<GWSource>
{
    client::Context upstream;

    mutable epicsMutex mutex;

    std::set<std::string> banHost, banPV;
    std::set<std::pair<std::string, std::string>> banHostPV;

    PyObject *handler = nullptr;

    // channel cache.  Indexed by upstream name
    std::map<std::string, std::shared_ptr<GWUpstream>> channels;

    static
    std::shared_ptr<GWSource> build(const client::Context& ctxt) {
        return std::shared_ptr<GWSource>(new GWSource(ctxt));
    }
    GWSource(const client::Context& ctxt)
        :upstream(ctxt)
    {}

    // for server::Source
    virtual void onSearch(Search &op) override final;
    virtual void onCreate(std::unique_ptr<server::ChannelControl> &&op) override final;

    GWSearchResult test(const std::string& usname);

    std::shared_ptr<GWChan> connect(const std::string& dsname,
                                    const std::string& usname,
                                    const std::shared_ptr<server::ChannelControl>& op);

    void sweep();
    void disconnect(const std::string& usname);
    void forceBan(const std::string& host, const std::string& usname);
    void clearBan();

    void cachePeek(std::set<std::string> &names) const;
};

} // namespace p4p

#endif // PVXS_GW_H
