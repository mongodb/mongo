/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/direct_connection_util.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop.h"
#include "mongo/db/s/sharding_api_d_params_gen.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_cluster_parameters_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MONGO_FAIL_POINT_DEFINE(skipDirectConnectionChecks);

namespace direct_connection_util {

void checkDirectShardOperationAllowed(OperationContext* opCtx, const NamespaceString& nss) {
    if (MONGO_unlikely(skipDirectConnectionChecks.shouldFail())) {
        return;
    }
    // Skip direct shard connection checks for namespaces which can have independent contents on
    // each shard. The direct shard connection check still applies to the sharding metadata
    // collection namespaces because those collections exist on a single, particular shard.
    if ((nss.isDbOnly() && nss.dbName().isInternalDb()) || nss.isShardLocalNamespace()) {
        return;
    }
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (ShardingState::get(opCtx)->enabled() && fcvSnapshot.isVersionInitialized() &&
        (feature_flags::gCheckForDirectShardOperations.isEnabled(fcvSnapshot) ||
         feature_flags::gFailOnDirectShardOperations.isEnabled(fcvSnapshot))) {
        bool clusterHasTwoOrMoreShards = [&]() {
            auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
            auto* clusterCardinalityParam =
                clusterParameters->get<ClusterParameterWithStorage<ShardedClusterCardinalityParam>>(
                    "shardedClusterCardinalityForDirectConns");
            return clusterCardinalityParam->getValue(boost::none).getHasTwoOrMoreShards();
        }();
        if (clusterHasTwoOrMoreShards || directConnectionChecksWithSingleShard.load()) {
            const bool authIsEnabled = AuthorizationManager::get(opCtx->getService()) &&
                AuthorizationManager::get(opCtx->getService())->isAuthEnabled();

            const bool directShardOperationsAllowed = !authIsEnabled ||
                (!CurOp::get(opCtx)->isStarted() || !CurOp::get(opCtx)->getCommand()) ||
                (AuthorizationSession::exists(opCtx->getClient()) &&
                 AuthorizationSession::get(opCtx->getClient())
                     ->isAuthorizedForActionsOnResource(
                         ResourcePattern::forClusterResource(nss.tenantId()),
                         ActionType::issueDirectShardOperations));

            if (!directShardOperationsAllowed) {
                ShardingStatistics::get(opCtx).unauthorizedDirectShardOperations.addAndFetch(1);
                if (feature_flags::gFailOnDirectShardOperations.isEnabled(fcvSnapshot) &&
                    clusterHasTwoOrMoreShards) {
                    // Atlas log ingestion requires a strict upper bound on the number of logs per
                    // hour. To abide by this, we only log this message once per hour and rely on
                    // the user assertion log (debug 1) otherwise.
                    const auto severity = ShardingState::get(opCtx)->directConnectionLogSeverity();
                    if (severity == logv2::LogSeverity::Warning()) {
                        LOGV2_DEBUG(8679600,
                                    logv2::LogSeverity::Error().toInt(),
                                    "You are connecting to a sharded cluster improperly by "
                                    "connecting directly to a shard. Please connect to the cluster "
                                    "via a router (mongos).",
                                    "command"_attr = CurOp::get(opCtx)->getCommand()->getName());
                    }
                    uasserted(
                        ErrorCodes::Unauthorized,
                        "You are connecting to a sharded cluster improperly by connecting directly "
                        "to a shard. Please connect to the cluster via a router (mongos).");
                } else if (feature_flags::gCheckForDirectShardOperations.isEnabled(fcvSnapshot) ||
                           (!clusterHasTwoOrMoreShards &&
                            directConnectionChecksWithSingleShard.load())) {
                    // Atlas log ingestion requires a strict upper bound on the number of logs per
                    // hour. To abide by this, we log the lower verbosity messages with a different
                    // log ID to prevent log ingestion from picking them up.
                    const auto severity = ShardingState::get(opCtx)->directConnectionLogSeverity();
                    LOGV2_DEBUG(
                        severity == logv2::LogSeverity::Warning() ? 7553700 : 8993900,
                        severity.toInt(),
                        "You are connecting to a sharded cluster improperly by connecting directly "
                        "to a shard. Please connect to the cluster via a router (mongos).",
                        "command"_attr = CurOp::get(opCtx)->getCommand()->getName());
                }
            }
        }
    }
}

}  // namespace direct_connection_util
}  // namespace mongo
