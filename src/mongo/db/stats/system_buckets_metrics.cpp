// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/stats/system_buckets_metrics.h"

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/stats/direct_system_buckets_access.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/version_context.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/string_map.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
auto& numCommandsTargetingSystemBuckets =
    *MetricBuilder<Counter64>("numCommandsTargetingSystemBuckets");
}  // namespace

// TODO SERVER-119235: Remove chunk-related commands once they support rawData.
static const StringDataSet kCommandsAllowedToTargetBuckets = {"moveChunk"sv,
                                                              "split"sv,
                                                              "mergeChunks"sv,
                                                              "moveRange"sv,
                                                              "clearJumboFlag"sv,
                                                              "cleanupOrphaned"sv,
                                                              "mergeAllChunksOnShard"sv,
                                                              "configureCollectionBalancing"sv,
                                                              "balancerCollectionStatus"sv};

// TODO SERVER-121176: Remove system.buckets metrics once 9.0 becomes last LTS
void SystemBucketsMetricsCommandHooks::onBeforeRun(OperationContext* opCtx,
                                                   CommandInvocation* invocation) {

    // Only care about timeseries buckets namespaces
    const auto& nss = invocation->ns();
    if (!nss.isTimeseriesBucketsCollection()) {
        return;
    }

    const auto& commandName = invocation->definition()->getName();
    if (kCommandsAllowedToTargetBuckets.contains(commandName)) {
        LOGV2_DEBUG(11923700,
                    2,
                    "Skipping system buckets metrics counter for allowed command",
                    "command"_attr = commandName,
                    "namespace"_attr = nss.toStringForErrorMsg());
        return;
    }

    const bool isInternal =
        // This command has been initiated by another command (e.g. DBDirectClient)
        isProcessInternalClient(*opCtx->getClient()) ||
        // This command comes from another node within the same cluster
        opCtx->getClient()->isInternalClient();

    const bool isRouter = opCtx->getService()->role().hasExclusively(ClusterRole::RouterServer);
    if (!isInternal) {
        // Only count external commands
        std::string_view appName;
        std::string_view driverName;
        if (auto clientMetadata = ClientMetadata::get(opCtx->getClient())) {
            appName = clientMetadata->getApplicationName();
            driverName = clientMetadata->getDriverName();
        }

        LOGV2_DEBUG(11259900,
                    _logSuppressor().toInt(),
                    "Received command targeting directly a system buckets namespace",
                    "command"_attr = commandName,
                    "isRouter"_attr = isRouter,
                    "namespace"_attr = nss.toStringForErrorMsg(),
                    "client"_attr = opCtx->getClient()->clientAddress(true),
                    "connId"_attr = opCtx->getClient()->getConnectionId(),
                    "appName"_attr = appName,
                    "driverName"_attr = driverName);
        numCommandsTargetingSystemBuckets.increment();
    }

    if (isRouter) {
        if (!isInternal) {
            // Signal to the shard that a user directly targeted system.buckets through the
            // router. The shard will check the feature flag and block if needed.
            isDirectSystemBucketsAccess(opCtx) = true;
        }
        return;
    }

    // On mongod, block if the user directly targeted system.buckets. This covers two cases:
    // 1. User connected directly to the shard (or replica set): isInternal=false
    // 2. User targeted system.buckets through the router: isDirectSystemBucketsAccess=true
    //    (isInternal=true because mongos connects as internal client, but the flag tells us
    //     the original request came from a user)
    // TODO SERVER-121176: keep the logic that blocks system.buckets access when removing the
    // metrics logic
    if ((!isInternal || isDirectSystemBucketsAccess(opCtx)) &&
        !allowDirectSystemBucketsAccess.load() &&
        gFeatureFlagBlockDirectSystemBucketsAccess.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        uasserted(ErrorCodes::CommandNotSupportedOnLegacyTimeseriesBucketsNamespace,
                  str::stream()
                      << "Command " << commandName << " on namespace " << nss.toStringForErrorMsg()
                      << " is not supported. Direct access to timeseries buckets namespaces is not"
                         " allowed anymore. Please target the main timeseries namespace instead."
                         " Use the rawData API to directly read timeseries buckets data in raw"
                         " format.");
    }
}

}  // namespace mongo
