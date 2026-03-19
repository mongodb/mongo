/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/stats/system_buckets_metrics.h"

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/stats/direct_system_buckets_access.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/version_context.h"
#include "mongo/util/string_map.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

// TODO SERVER-119235: Remove chunk-related commands once they support rawData.
static const StringDataSet kCommandsAllowedToTargetBuckets = {"moveChunk"_sd,
                                                              "split"_sd,
                                                              "mergeChunks"_sd,
                                                              "moveRange"_sd,
                                                              "clearJumboFlag"_sd,
                                                              "cleanupOrphaned"_sd,
                                                              "mergeAllChunksOnShard"_sd,
                                                              "configureCollectionBalancing"_sd,
                                                              "balancerCollectionStatus"_sd};

SystemBucketsMetricsCommandHooks::SystemBucketsMetricsCommandHooks() {
    _commandsExecuted = &*MetricBuilder<Counter64>("numCommandsTargetingSystemBuckets");
}

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
        LOGV2_DEBUG(11259900,
                    _logSuppressor().toInt(),
                    "Received command targeting directly a system buckets namespace",
                    "command"_attr = commandName,
                    "isRouter"_attr = isRouter,
                    "namespace"_attr = nss.toStringForErrorMsg(),
                    "client"_attr = opCtx->getClient()->clientAddress(true),
                    "connId"_attr = opCtx->getClient()->getConnectionId());
        _commandsExecuted->increment();
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
