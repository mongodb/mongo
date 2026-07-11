// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/notify_range_deleter_after_move_range_recovery.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/future_impl.h"

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

namespace mongo {

void asyncNotifyRangeDeleterAfterMoveRangeRecovery(ServiceContext* serviceContext) {
    if (!serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        return;
    }

    const auto term = repl::ReplicationCoordinator::get(serviceContext)->getTerm();

    // The Grid is not initialized when there are no shards in the cluster yet (e.g. before this
    // node has been added to a shard). In that case there cannot be any MoveRangeCoordinators to
    // wait for, so there is nothing to recover: immediately mark the recovery job complete and
    // return. We also cannot proceed past this point without the Grid, since we rely on its
    // executor pool below.
    if (!Grid::get(serviceContext)->isInitialized()) {
        RangeDeleterService::get(serviceContext)->notifyRecoveryJobComplete(term);
        return;
    }

    ExecutorFuture<void>{Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor()}
        .then([serviceContext, term] {
            ThreadClient tc{"MoveRangeCoordinatorRecoveryWait", serviceContext->getService()};
            auto uniqueOpCtx = tc->makeOperationContext();
            auto* notifyOpCtx = uniqueOpCtx.get();

            boost::optional<rss::consensus::WriteIntentGuard> writeGuard;
            if (gFeatureFlagIntentRegistration.isEnabled()) {
                writeGuard.emplace(notifyOpCtx);
            }
            notifyOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            try {
                ShardingCoordinatorService::getService(notifyOpCtx)
                    ->waitForCoordinatorsOfGivenTypeToComplete(notifyOpCtx,
                                                               CoordinatorTypeEnum::kMoveRange);
            } catch (const DBException& ex) {
                // The only expected reasons for this wait to fail are this node stepping down (via
                // notifyOpCtx's own interruption or a coordinator's completion future resolving
                // with a not-primary error) or shutdown. Anything else is a bug.
                tassert(13021503,
                        "Unexpected error while waiting for MoveRangeCoordinator step-up recovery",
                        ErrorCodes::isA<ErrorCategory::NotPrimaryError>(ex.code()) ||
                            ErrorCodes::isA<ErrorCategory::ShutdownError>(ex.code()) ||
                            ErrorCodes::isA<ErrorCategory::CancellationError>(ex.code()));
                LOGV2_DEBUG(13021500,
                            2,
                            "Interrupted while waiting for MoveRangeCoordinator step-up recovery",
                            "term"_attr = term,
                            "error"_attr = redact(ex));
            }
        })
        .onCompletion([serviceContext, term](const auto&) {
            RangeDeleterService::get(serviceContext)->notifyRecoveryJobComplete(term);
            LOGV2_DEBUG(13021501,
                        2,
                        "Finished waiting for MoveRangeCoordinator step-up recovery",
                        "term"_attr = term);
        })
        .getAsync([](const auto&) {});
}

}  // namespace mongo
