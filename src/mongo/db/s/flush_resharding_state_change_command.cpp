/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/flush_resharding_state_change_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failFlushReshardingStateChange);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

/**
 * Returns true if _flushReshardingStateChange command should retry refreshing sharding metadata
 * upon getting the given error.
 */
bool shouldRetryOnRefreshError(const Status& status) {
    if (status == ErrorCodes::NamespaceNotFound) {
        // The collection has been dropped.
        return false;
    }
    // We need to retry on WriteConcernTimeout errors since doing a sharding metadata refresh
    // involves performing a noop write with majority write concern with a timeout. We need to retry
    // on snapshot errors since doing a sharding metadata refresh involves running an aggregation
    // over the config.collections and config.chunks collections with snapshot read concern. The
    // catalog cache does retry on snapshot errors but the number of retries is capped.
    return primary_only_service_helpers::kDefaultRetryabilityPredicate(status) ||
        status == ErrorCodes::WriteConcernTimeout || status.isA<ErrorCategory::SnapshotError>();
}

class FlushReshardingStateChangeCmd final : public TypedCommand<FlushReshardingStateChangeCmd> {
public:
    using Request = _flushReshardingStateChange;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command used by the resharding coordinator to flush state changes to the "
               "participant shards while the critical section is active.";
    }

    bool adminOnly() const override {
        return true;
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        UUID reshardingUUID() const {
            return request().getReshardingUUID();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        void typedRun(OperationContext* opCtx) {
            auto const shardingState = ShardingState::get(opCtx);
            shardingState->assertCanAcceptShardedCommands();

            uassert(ErrorCodes::IllegalOperation,
                    "Can't issue _flushReshardingStateChange from 'eval'",
                    !opCtx->getClient()->isInDirectClient());

            uassert(ErrorCodes::IllegalOperation,
                    "Can't call _flushReshardingStateChange if in read-only mode",
                    !opCtx->readOnly());

            // We use the fixed executor here since it may cause the thread to block. This would
            // cause potential liveness issues since the arbitrary executor is a NetworkInterfaceTL
            // executor in sharded clusters and that executor is one that executes networking
            // operations.
            AsyncTry([svcCtx = opCtx->getServiceContext(), nss = ns(), numTries = 0]() mutable {
                ThreadClient tc("FlushReshardingStateChange",
                                svcCtx->getService(ClusterRole::ShardServer));
                auto opCtx = tc->makeOperationContext();

                auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
                if (!replCoord->getMemberState().primary()) {
                    LOGV2(10795200,
                          "Stop refreshing sharding metadata in _flushReshardingStateChange since "
                          "this node is no longer a primary",
                          "numTries"_attr = numTries);
                    return;
                }

                numTries++;
                LOGV2_DEBUG(10795201,
                            1,
                            "Start refreshing sharding metadata in _flushReshardingStateChange",
                            "numTries"_attr = numTries);

                auto& shardingStatistics = ShardingStatistics::get(opCtx.get());
                shardingStatistics.countFlushReshardingStateChangeTotalShardingMetadataRefreshes
                    .addAndFetch(1);

                boost::optional<Status> mockStatus;
                failFlushReshardingStateChange.execute([&](const BSONObj& data) {
                    const auto& errorCode = data.getIntField("errorCode");
                    mockStatus =
                        Status(ErrorCodes::Error(errorCode),
                               "Failing refresh in _flushReshardingStateChange due to failpoint");
                });

                auto refreshStatus = mockStatus
                    ? *mockStatus
                    : FilteringMetadataCache::get(opCtx.get())
                          ->onCollectionPlacementVersionMismatch(
                              opCtx.get(), nss, boost::none /* chunkVersionReceived */);

                if (refreshStatus.isOK()) {
                    shardingStatistics
                        .countFlushReshardingStateChangeSuccessfulShardingMetadataRefreshes
                        .addAndFetch(1);
                } else {
                    shardingStatistics
                        .countFlushReshardingStateChangeFailedShardingMetadataRefreshes.addAndFetch(
                            1);
                }

                uassertStatusOK(refreshStatus);
                LOGV2_DEBUG(10795202,
                            1,
                            "Finished refreshing sharding metadata in _flushReshardingStateChange",
                            "numTries"_attr = numTries);
            })
                .until([](Status status) {
                    if (!status.isOK()) {
                        LOGV2_WARNING(5808100,
                                      "Error on deferred _flushReshardingStateChange execution",
                                      "error"_attr = redact(status));
                    }
                    return status.isOK() || !shouldRetryOnRefreshError(status);
                })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                    CancellationToken::uncancelable())
                .getAsync([](auto) {});

            // Ensure the command isn't run on a stale primary.
            resharding::doNoopWrite(opCtx, "_flushReshardingStateChange no-op", ns());
        }
    };
};
MONGO_REGISTER_COMMAND(FlushReshardingStateChangeCmd).forShard();

}  // namespace
}  // namespace mongo
