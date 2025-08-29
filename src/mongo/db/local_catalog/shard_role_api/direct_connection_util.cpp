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

#include "mongo/db/local_catalog/shard_role_api/direct_connection_util.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/sharding_api_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace direct_connection_util {
namespace {

MONGO_FAIL_POINT_DEFINE(skipDirectConnectionChecks);

bool shouldSkipDirectShardOpChecks(OperationContext* opCtx, const NamespaceString& nss) {
    if (MONGO_unlikely(skipDirectConnectionChecks.shouldFail())) {
        return true;
    }
    // Skip direct shard connection checks for namespaces which can have independent contents on
    // each shard. The direct shard connection check still applies to the sharding metadata
    // collection namespaces because those collections exist on a single, particular shard.
    if ((nss.isDbOnly() && nss.dbName().isInternalDb()) || nss.isShardLocalNamespace()) {
        return true;
    }
    // Skip direct shard connection checks for commands which explicitly request skipping these
    // checks. There should be very few cases in which this is true.
    if (OperationShardingState::get(opCtx).shouldSkipDirectConnectionChecks()) {
        return true;
    }
    // Skip direct shard connections checks when the corresponding server parameter is set. This
    // parameter should be active in rare cases, like restoring a sharded cluster as a replica set.
    if (disableDirectShardDDLOperations.load()) {
        return true;
    }
    // Skip direct shard connection checks when the sharding state is disabled, this prevents
    // blocking direct operations on replica sets.
    if (!ShardingState::get(opCtx)->enabled()) {
        return true;
    }
    return false;
}

bool isAuthorizedForDirectShardOperation(OperationContext* opCtx, const NamespaceString& nss) {
    const bool authIsEnabled = AuthorizationManager::get(opCtx->getService()) &&
        AuthorizationManager::get(opCtx->getService())->isAuthEnabled();
    return !authIsEnabled ||
        (AuthorizationSession::exists(opCtx->getClient()) &&
         AuthorizationSession::get(opCtx->getClient())
             ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(nss.tenantId()),
                                                ActionType::issueDirectShardOperations));
}

}  // namespace

void checkDirectShardDDLAllowed(OperationContext* opCtx, const NamespaceString& nss) {
    if (shouldSkipDirectShardOpChecks(opCtx, nss)) {
        return;
    }

    if (!isAuthorizedForDirectShardOperation(opCtx, nss)) {
        ShardingStatistics::get(opCtx).unauthorizedDirectShardOperations.addAndFetch(1);
        uasserted(
            ErrorCodes::Unauthorized,
            str::stream() << "DDL operations are not allowed via direct shard connection. Please "
                             "connect to the cluster via a router (mongos). Namespace: "
                          << nss.toStringForErrorMsg());
    }
}

void checkDirectShardOperationAllowed(OperationContext* opCtx, const NamespaceString& nss) {
    if (shouldSkipDirectShardOpChecks(opCtx, nss)) {
        return;
    }

    bool clusterHasTwoOrMoreShards = [&]() {
        auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
        auto* clusterCardinalityParam =
            clusterParameters->get<ClusterParameterWithStorage<ShardedClusterCardinalityParam>>(
                "shardedClusterCardinalityForDirectConns");
        return clusterCardinalityParam->getValue(boost::none).getHasTwoOrMoreShards();
    }();
    if (clusterHasTwoOrMoreShards || directConnectionChecksWithSingleShard.load()) {
        // Internal lock acquisitions don't set authentication info on the opCtx, so we rely on
        // CurOp to check that the command came through the SEP.
        bool curOpInitialized = CurOp::get(opCtx)->isStarted() && CurOp::get(opCtx)->getCommand();
        if (curOpInitialized && !isAuthorizedForDirectShardOperation(opCtx, nss)) {
            ShardingStatistics::get(opCtx).unauthorizedDirectShardOperations.addAndFetch(1);
            static constexpr char errorMsg[] =
                "You are connecting to a sharded cluster improperly by connecting directly "
                "to "
                "a shard. Please connect to the cluster via a router (mongos).";
            if (clusterHasTwoOrMoreShards) {
                // Atlas log ingestion requires a strict upper bound on the number of logs
                // per hour. To abide by this, we only log this message once per hour and
                // rely on the user assertion log (debug 1) otherwise.
                const auto severity = ShardingState::get(opCtx)->directConnectionLogSeverity();
                if (severity == logv2::LogSeverity::Warning()) {
                    LOGV2_DEBUG(8679600,
                                logv2::LogSeverity::Error().toInt(),
                                errorMsg,
                                logAttrs(nss),
                                "command"_attr = CurOp::get(opCtx)->getCommand()->getName());
                }
                uasserted(ErrorCodes::Unauthorized,
                          str::stream() << errorMsg << " Command: "
                                        << CurOp::get(opCtx)->getCommand()->getName()
                                        << ", Namespace: " << nss.toStringForErrorMsg());
            } else if (directConnectionChecksWithSingleShard.load()) {
                // Atlas log ingestion requires a strict upper bound on the number of logs
                // per hour. To abide by this, we log the lower verbosity messages with a
                // different log ID to prevent log ingestion from picking them up.
                const auto severity = ShardingState::get(opCtx)->directConnectionLogSeverity();
                LOGV2_DEBUG(severity == logv2::LogSeverity::Warning() ? 7553700 : 8993900,
                            severity.toInt(),
                            errorMsg,
                            logAttrs(nss),
                            "command"_attr = CurOp::get(opCtx)->getCommand()->getName());
            }
        }
    }
}

}  // namespace direct_connection_util
}  // namespace mongo
