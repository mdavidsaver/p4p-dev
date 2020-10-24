#ifndef PVXS_GW_H
#define PVXS_GW_H

#include <atomic>

#include "p4p.h"

#include <pvxs/source.h>
#include <pvxs/sharedpv.h>
#include <pvxs/client.h>

namespace p4p {
using namespace pvxs;

enum GWSearchResult {
    GWSearchIgnore,
    GWSearchClaim,
    GWSearchBanHost,
    GWSearchBanPV,
    GWSearchBanHostPV,
};

struct GWUpstream {
    const std::string usname;
    client::Context upstream;

    std::shared_ptr<client::Connect> connector;

    // time in msec
    std::atomic<unsigned> get_holdoff{};

    bool gcmark = false;

    GWUpstream(const std::string& usname, client::Context& ctxt)
        :usname(usname)
        ,upstream(ctxt)
        ,connector(upstream.connect(usname).exec())
    {
        // TODO: connector handle onDisconnect()
    }
};

struct GWChan {
    const std::string dsname;
    const std::shared_ptr<GWUpstream> us;

    // Use atomic access.
    // binary flags
    std::atomic<bool> allow_put{},
                      allow_rpc{},
                      allow_uncached{},
                      audit{};

    //std::shared_ptr<client::Operation> subscription;

    GWChan(const std::string& dsname, const std::shared_ptr<GWUpstream>& upstream)
        :dsname(dsname)
        ,us(upstream)
    {
    }
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
