/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/service_entry_point_mongod.h"

#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/curop.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/s/scoped_operation_completion_sharding_actions.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/s/stale_exception.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

class ServiceEntryPointMongod::Hooks final : public ServiceEntryPointCommon::Hooks {
public:
    bool lockedForWriting() const override {
        return mongo::lockedForWriting();
    }

    void setPrepareConflictBehaviorForReadConcern(
        OperationContext* opCtx, const CommandInvocation* invocation) const override {
        // Some read commands can safely ignore prepare conflicts by default because they do not
        // require snapshot isolation and do not conflict with concurrent writes. We also give these
        // operations permission to write, as this may be required for queries that spill using the
        // storage engine. The kIgnoreConflictsAllowWrites setting suppresses an assertion in the
        // storage engine that prevents operations that ignore prepare conflicts from also writing.
        const auto prepareConflictBehavior = invocation->canIgnorePrepareConflicts()
            ? PrepareConflictBehavior::kIgnoreConflictsAllowWrites
            : PrepareConflictBehavior::kEnforce;
        mongo::setPrepareConflictBehaviorForReadConcern(
            opCtx, repl::ReadConcernArgs::get(opCtx), prepareConflictBehavior);
    }

    void waitForReadConcern(OperationContext* opCtx,
                            const CommandInvocation* invocation,
                            const OpMsgRequest& request) const override {
        Status rcStatus = mongo::waitForReadConcern(opCtx,
                                                    repl::ReadConcernArgs::get(opCtx),
                                                    request.getDatabase(),
                                                    invocation->allowsAfterClusterTime());

        if (!rcStatus.isOK()) {
            if (ErrorCodes::isExceededTimeLimitError(rcStatus.code())) {
                const int debugLevel =
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 0 : 2;
                LOGV2_DEBUG(21975,
                            debugLevel,
                            "Command on database {db} timed out waiting for read concern to be "
                            "satisfied. Command: {command}. Info: {error}",
                            "Command timed out waiting for read concern to be satisfied",
                            "db"_attr = request.getDatabase(),
                            "command"_attr =
                                redact(ServiceEntryPointCommon::getRedactedCopyForLogging(
                                    invocation->definition(), request.body)),
                            "error"_attr = redact(rcStatus));
            }

            uassertStatusOK(rcStatus);
        }
    }

    void waitForSpeculativeMajorityReadConcern(OperationContext* opCtx) const override {
        auto speculativeReadInfo = repl::SpeculativeMajorityReadInfo::get(opCtx);
        if (!speculativeReadInfo.isSpeculativeRead()) {
            return;
        }
        uassertStatusOK(mongo::waitForSpeculativeMajorityReadConcern(opCtx, speculativeReadInfo));
    }


    void waitForWriteConcern(OperationContext* opCtx,
                             const CommandInvocation* invocation,
                             const repl::OpTime& lastOpBeforeRun,
                             BSONObjBuilder& commandResponseBuilder) const override {

        // Prevent waiting for writeConcern if the command is changing an unreplicated namespace.
        invariant(invocation);
        if (!invocation->ns().isReplicated()) {
            return;
        }

        // Do not increase consumption metrics during wait for write concern, as in serverless this
        // might cause a tenant to be billed for reading the oplog entry (which might be of
        // considerable size) of another tenant.
        ResourceConsumption::PauseMetricsCollectorBlock pauseMetricsCollection(opCtx);

        auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        auto waitForWriteConcernAndAppendStatus = [&]() {
            WriteConcernResult res;
            auto waitForWCStatus =
                mongo::waitForWriteConcern(opCtx, lastOpAfterRun, opCtx->getWriteConcern(), &res);

            CommandHelpers::appendCommandWCStatus(commandResponseBuilder, waitForWCStatus, res);
        };

        // If lastOp has changed, then a write has been done by this client. This timestamp is
        // sufficient for waiting for write concern.
        if (lastOpAfterRun != lastOpBeforeRun) {
            invariant(lastOpAfterRun > lastOpBeforeRun);
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // If an error occurs after performing a write but before waiting for write concern and
        // returning to the client, the driver may retry an operation that has already been
        // completed, resulting in a no-op. The no-op has to wait for the write concern nonetheless,
        // because acknowledgement from secondaries might still be pending. Given that the timestamp
        // of the original operation that performed the write is not available, the best
        // approximation is to use the system’s last op time, which is guaranteed to be >= than the
        // original op time.

        // Ensures that if we tried to do a write, we wait for write concern, even if that write was
        // a noop. We do not need to update this for multi-document transactions as read-only/noop
        // transactions will do a noop write at commit time, which should have incremented the
        // lastOp. And speculative majority semantics dictate that "abortTransaction" should not
        // wait for write concern on operations the transaction observed.
        if (opCtx->lockState()->wasGlobalLockTakenForWrite() &&
            !opCtx->inMultiDocumentTransaction()) {
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // Waits for write concern if we tried to explicitly set the lastOp forward but lastOp was
        // already up to date. We still want to wait for write concern on the lastOp. This is
        // primarily to make sure back to back retryable write retries still wait for write concern.
        //
        // WARNING: Retryable writes that expect to wait for write concern on retries must ensure
        // this is entered by calling setLastOp() or setLastOpToSystemLastOpTime().
        if (repl::ReplClientInfo::forClient(opCtx->getClient())
                .lastOpWasSetExplicitlyByClientForCurrentOperation(opCtx)) {
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // If no write was attempted and the client's lastOp was not changed by the current network
        // operation then we skip waiting for writeConcern.
    }

    void waitForLinearizableReadConcern(OperationContext* opCtx) const override {
        // When a linearizable read command is passed in, check to make sure we're reading
        // from the primary.
        if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
            repl::ReadConcernLevel::kLinearizableReadConcern) {
            uassertStatusOK(mongo::waitForLinearizableReadConcern(opCtx, Milliseconds::zero()));
        }
    }

    void uassertCommandDoesNotSpecifyWriteConcern(const BSONObj& cmd) const override {
        if (commandSpecifiesWriteConcern(cmd)) {
            uasserted(ErrorCodes::InvalidOptions, "Command does not support writeConcern");
        }
    }

    void attachCurOpErrInfo(OperationContext* opCtx, const BSONObj& replyObj) const override {
        CurOp::get(opCtx)->debug().errInfo = getStatusFromCommandResult(replyObj);
    }

    void appendReplyMetadata(OperationContext* opCtx,
                             const OpMsgRequest& request,
                             BSONObjBuilder* metadataBob) const override {
        auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
        const bool isReplSet =
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

        if (isReplSet) {
            // Attach our own last opTime.
            repl::OpTime lastOpTimeFromClient =
                repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            replCoord->prepareReplMetadata(request.body, lastOpTimeFromClient, metadataBob);
        }
    }

    bool refreshDatabase(OperationContext* opCtx, const StaleDbRoutingVersion& se) const
        noexcept override {
        return onDbVersionMismatchNoExcept(opCtx, se.getDb(), se.getVersionReceived()).isOK();
    }

    bool refreshCollection(OperationContext* opCtx, const StaleConfigInfo& se) const
        noexcept override {
        return onShardVersionMismatchNoExcept(opCtx, se.getNss(), se.getVersionReceived()).isOK();
    }

    bool refreshCatalogCache(OperationContext* opCtx,
                             const ShardCannotRefreshDueToLocksHeldInfo& refreshInfo) const
        noexcept override {
        return Grid::get(opCtx)
            ->catalogCache()
            ->getCollectionRoutingInfo(opCtx, refreshInfo.getNss())
            .isOK();
    }

    void handleReshardingCriticalSectionMetrics(OperationContext* opCtx,
                                                const StaleConfigInfo& se) const noexcept override {
        resharding_metrics::onCriticalSectionError(opCtx, se);
    }

    // The refreshDatabase, refreshCollection, and refreshCatalogCache methods may have modified the
    // locker state, in particular the flags which say if the operation took a write lock or shared
    // lock.  This will cause mongod to perhaps erroneously check for write concern when no writes
    // were done, or unnecessarily kill a read operation.  If we re-use the opCtx to retry command
    // execution, we must reset the locker state.
    void resetLockerState(OperationContext* opCtx) const noexcept override {
        // It is necessary to lock the client to change the Locker on the OperationContext.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        invariant(!opCtx->lockState()->isLocked());
        opCtx->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()), lk);
    }

    std::unique_ptr<PolymorphicScoped> scopedOperationCompletionShardingActions(
        OperationContext* opCtx) const override {
        return std::make_unique<ScopedOperationCompletionShardingActions>(opCtx);
    }
};

Future<DbResponse> ServiceEntryPointMongod::handleRequest(OperationContext* opCtx,
                                                          const Message& m) noexcept {
    return ServiceEntryPointCommon::handleRequest(opCtx, m, std::make_unique<Hooks>());
}

}  // namespace mongo
