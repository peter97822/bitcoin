#include <node/peerman_args.h>

#include <common/args.h>
#include <net_processing.h>

namespace node {

void ApplyArgsManOptions(const ArgsManager& argsman, PeerManager::Options& options)
{
    if (auto value{argsman.GetBoolArg("-txreconciliation")}) options.reconcile_txs = *value;

    if (auto value{argsman.GetIntArg("-maxorphantx")}) {
        options.max_orphan_txs = uint32_t(std::max(int64_t{0}, *value));
    }
}

} // namespace node

