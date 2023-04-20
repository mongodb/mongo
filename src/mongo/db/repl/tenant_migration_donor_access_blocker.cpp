/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/logv2/log.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(tenantMigrationDonorAllowsNonTimestampedReads);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

// Commands that are not allowed to run against the donor after a committed migration so that we
// typically provide read-your-own-write guarantees for primary reads across tenant migrations.
const StringMap<int> commandDenyListAfterMigration = {
    {"find", 1},
    {"count", 1},
    {"distinct", 1},
    {"aggregate", 1},
    {"mapReduce", 1},
    {"mapreduce", 1},
    {"findAndModify", 1},
    {"findandmodify", 1},
    {"listCollections", 1},
    {"listIndexes", 1},
    {"update", 1},
    {"delete", 1},
};

}  // namespace

TenantMigrationDonorAccessBlocker::TenantMigrationDonorAccessBlocker(ServiceContext* serviceContext,
                                                                     const UUID& migrationId)
    : TenantMigrationAccessBlocker(BlockerType::kDonor, migrationId),
      _serviceContext(serviceContext) {}

Status TenantMigrationDonorAccessBlocker::checkIfCanWrite(Timestamp writeTs) {
    stdx::lock_guard<Latch> lg(_mutex);

    switch (_state.getState()) {
        case BlockerState::State::kAllow:
            // As a sanity check, we track the highest allowed write timestamp to ensure no
            // writes are allowed with a timestamp higher than the block timestamp.
            _highestAllowedWriteTimestamp = std::max(writeTs, _highestAllowedWriteTimestamp);
            [[fallthrough]];
        case BlockerState::State::kAborted:
            return Status::OK();
        case BlockerState::State::kBlockWrites:
        case BlockerState::State::kBlockWritesAndReads:
            return {TenantMigrationConflictInfo(getMigrationId(), shared_from_this()),
                    "Write must block until this tenant migration commits or aborts"};
        case BlockerState::State::kReject:
            return {ErrorCodes::TenantMigrationCommitted,
                    "Write must be re-routed to the new owner of this tenant"};
        default:
            MONGO_UNREACHABLE;
    }
}

Status TenantMigrationDonorAccessBlocker::waitUntilCommittedOrAborted(OperationContext* opCtx) {
    // Source to cancel the timeout if the operation completed in time.
    CancellationSource cancelTimeoutSource;
    auto executor = TenantMigrationAccessBlockerRegistry::get(_serviceContext)
                        .getAsyncBlockingOperationsExecutor();
    std::vector<ExecutorFuture<void>> futures;

    futures.emplace_back(_onCompletion().thenRunOn(executor));

    if (opCtx->hasDeadline()) {
        auto deadlineReachedFuture =
            executor->sleepUntil(opCtx->getDeadline(), cancelTimeoutSource.token());
        // The timeout condition is optional with index #1.
        futures.push_back(std::move(deadlineReachedFuture));
    }

    auto waitResult = whenAny(std::move(futures)).getNoThrow(opCtx);
    if (!waitResult.isOK()) {
        return waitResult.getStatus();
    }
    const auto& [status, idx] = waitResult.getValue();

    if (idx == 0) {
        // _onCompletion() finished first.
        cancelTimeoutSource.cancel();
        return status;
    } else if (idx == 1) {
        // Deadline finished first, return error
        return Status(
            opCtx->getTimeoutError(),
            "Operation timed out waiting for an internal data migration to commit or abort");
    }
    MONGO_UNREACHABLE;
}

SharedSemiFuture<void> TenantMigrationDonorAccessBlocker::getCanReadFuture(OperationContext* opCtx,
                                                                           StringData command) {
    // Exclude internal client requests
    if (tenant_migration_access_blocker::shouldExcludeRead(opCtx)) {
        LOGV2_DEBUG(6397500,
                    1,
                    "Internal tenant read got excluded from the MTAB filtering",
                    "migrationId"_attr = getMigrationId(),
                    "opId"_attr = opCtx->getOpID());
        return SharedSemiFuture<void>();
    }

    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto readTimestamp = [opCtx, &readConcernArgs]() -> boost::optional<Timestamp> {
        if (auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime()) {
            return afterClusterTime->asTimestamp();
        }
        if (auto atClusterTime = readConcernArgs.getArgsAtClusterTime()) {
            return atClusterTime->asTimestamp();
        }
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            return repl::StorageInterface::get(opCtx)->getPointInTimeReadTimestamp(opCtx);
        }
        return boost::none;
    }();

    stdx::lock_guard<Latch> lk(_mutex);

    switch (_state.getState()) {
        case BlockerState::State::kAllow:
        case BlockerState::State::kBlockWrites:
        case BlockerState::State::kAborted:
            return SharedSemiFuture<void>();

        case BlockerState::State::kBlockWritesAndReads:
            if (!readTimestamp || *readTimestamp < *_blockTimestamp) {
                return SharedSemiFuture<void>();
            } else {
                _stats.numBlockedReads.addAndFetch(1);
                return _transitionOutOfBlockingPromise.getFuture();
            }

        case BlockerState::State::kReject:
            if (!readTimestamp) {
                if (MONGO_unlikely(tenantMigrationDonorAllowsNonTimestampedReads.shouldFail()) ||
                    commandDenyListAfterMigration.find(command) ==
                        commandDenyListAfterMigration.end()) {
                    return SharedSemiFuture<void>();
                }
            } else if (*readTimestamp < *_blockTimestamp) {
                return SharedSemiFuture<void>();
            }

            return SharedSemiFuture<void>(
                Status(ErrorCodes::TenantMigrationCommitted,
                       "Read must be re-routed to the new owner of this tenant"));

        default:
            MONGO_UNREACHABLE;
    }
}

Status TenantMigrationDonorAccessBlocker::checkIfLinearizableReadWasAllowed(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_state.isReject()) {
        return {ErrorCodes::TenantMigrationCommitted,
                "Read must be re-routed to the new owner of this tenant"};
    }
    return Status::OK();
}

Status TenantMigrationDonorAccessBlocker::checkIfCanBuildIndex() {
    stdx::lock_guard<Latch> lg(_mutex);
    switch (_state.getState()) {
        case BlockerState::State::kAllow:
        case BlockerState::State::kBlockWrites:
        case BlockerState::State::kBlockWritesAndReads:
            return {TenantMigrationConflictInfo(getMigrationId(), shared_from_this()),
                    "Index build must block until tenant migration is committed or aborted."};
        case BlockerState::State::kReject:
            return {ErrorCodes::TenantMigrationCommitted,
                    "Index build must be re-routed to the new owner of this tenant"};
        case BlockerState::State::kAborted:
            return Status::OK();
    }
    MONGO_UNREACHABLE;
}

void TenantMigrationDonorAccessBlocker::startBlockingWrites() {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(5093800, "Starting to block writes", "migrationId"_attr = getMigrationId());

    invariant(!_blockTimestamp);
    invariant(!_commitOpTime);
    invariant(!_abortOpTime);

    _state.transitionTo(BlockerState::State::kBlockWrites);
}

void TenantMigrationDonorAccessBlocker::startBlockingReadsAfter(const Timestamp& blockTimestamp) {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(5093801,
          "Starting to block reads after blockTimestamp",
          "migrationId"_attr = getMigrationId(),
          "blockTimestamp"_attr = blockTimestamp);

    invariant(!_blockTimestamp);
    invariant(!_commitOpTime);
    invariant(!_abortOpTime);

    invariant(
        blockTimestamp > _highestAllowedWriteTimestamp,
        str::stream() << "The block timestamp must be higher than the timestamp of any allowed "
                         "write, blockTimestamp: "
                      << blockTimestamp.toString() << ", highestAllowedWriteTimestamp: "
                      << _highestAllowedWriteTimestamp.toString());

    _state.transitionTo(BlockerState::State::kBlockWritesAndReads);
    _blockTimestamp = blockTimestamp;
}

void TenantMigrationDonorAccessBlocker::rollBackStartBlocking() {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(!_commitOpTime);
    invariant(!_abortOpTime);

    _state.transitionTo(BlockerState::State::kAllow);
    _blockTimestamp.reset();
    _transitionOutOfBlockingPromise.setFrom(Status::OK());
}

void TenantMigrationDonorAccessBlocker::interrupt() {
    stdx::unique_lock<Latch> lk(_mutex);
    const Status status(
        ErrorCodes::Interrupted,
        "Blocked read or write interrupted while waiting for tenant migration to commit or abort");
    if (!_transitionOutOfBlockingPromise.getFuture().isReady()) {
        _transitionOutOfBlockingPromise.setFrom(status);
    }
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void TenantMigrationDonorAccessBlocker::setCommitOpTime(OperationContext* opCtx,
                                                        repl::OpTime opTime) {
    {
        stdx::lock_guard<Latch> lg(_mutex);

        invariant(_state.isBlockWritesAndReads());
        invariant(!_commitOpTime);
        invariant(!_abortOpTime);

        _commitOpTime = opTime;
    }

    LOGV2(5107300,
          "Starting to wait for commit OpTime to be majority-committed",
          "migrationId"_attr = getMigrationId(),
          "commitOpTime"_attr = opTime);

    if (opTime > repl::ReplicationCoordinator::get(opCtx)->getCurrentCommittedSnapshotOpTime()) {
        return;
    }

    stdx::unique_lock<Latch> lk(_mutex);
    if (_completionPromise.getFuture().isReady()) {
        // onMajorityCommitPointUpdate() was called during the time that this thread is not holding
        // the lock.
        return;
    }
    _onMajorityCommitCommitOpTime(lk);
}

void TenantMigrationDonorAccessBlocker::setAbortOpTime(OperationContext* opCtx,
                                                       repl::OpTime opTime) {
    {
        stdx::lock_guard<Latch> lg(_mutex);

        invariant(!_commitOpTime);
        invariant(!_abortOpTime);

        _abortOpTime = opTime;
    }

    LOGV2(5107301,
          "Starting to wait for abort OpTime to be majority-committed",
          "migrationId"_attr = getMigrationId(),
          "abortOpTime"_attr = opTime);

    if (opTime > repl::ReplicationCoordinator::get(opCtx)->getCurrentCommittedSnapshotOpTime()) {
        return;
    }

    stdx::unique_lock<Latch> lk(_mutex);
    if (_completionPromise.getFuture().isReady()) {
        // onMajorityCommitPointUpdate() was called during the time that this thread is not holding
        // the lock.
        return;
    }
    _onMajorityCommitAbortOpTime(lk);
}

void TenantMigrationDonorAccessBlocker::onMajorityCommitPointUpdate(repl::OpTime opTime) {
    stdx::unique_lock<Latch> lk(_mutex);

    if (_completionPromise.getFuture().isReady()) {
        return;
    }

    invariant(!_commitOpTime || !_abortOpTime);

    if (_commitOpTime && _commitOpTime <= opTime) {
        _onMajorityCommitCommitOpTime(lk);
    } else if (_abortOpTime && _abortOpTime <= opTime) {
        _onMajorityCommitAbortOpTime(lk);
    }
}

void TenantMigrationDonorAccessBlocker::_onMajorityCommitCommitOpTime(
    stdx::unique_lock<Latch>& lk) {
    invariant(_blockTimestamp);
    invariant(_commitOpTime);
    invariant(!_abortOpTime);

    _state.transitionTo(BlockerState::State::kReject);
    Status error{ErrorCodes::TenantMigrationCommitted,
                 "Write or read must be re-routed to the new owner of this tenant"};
    _completionPromise.setError(error);
    _transitionOutOfBlockingPromise.setFrom(error);

    lk.unlock();
    LOGV2(5093803,
          "Finished waiting for commit OpTime to be majority-committed",
          "migrationId"_attr = getMigrationId());
}

void TenantMigrationDonorAccessBlocker::_onMajorityCommitAbortOpTime(stdx::unique_lock<Latch>& lk) {
    invariant(!_commitOpTime);
    invariant(_abortOpTime);

    _state.transitionTo(BlockerState::State::kAborted);
    _transitionOutOfBlockingPromise.setFrom(Status::OK());
    _completionPromise.setError({ErrorCodes::TenantMigrationAborted, "Tenant migration aborted"});

    lk.unlock();
    LOGV2(5093805,
          "Finished waiting for abort OpTime to be majority-committed",
          "migrationId"_attr = getMigrationId());
}

void TenantMigrationDonorAccessBlocker::appendInfoForServerStatus(BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(!_commitOpTime || !_abortOpTime);

    getMigrationId().appendToBuilder(builder, "migrationId");
    builder->append("state", _state.toString());
    if (_blockTimestamp) {
        builder->append("blockTimestamp", _blockTimestamp.value());
    }
    if (_commitOpTime) {
        builder->append("commitOpTime", _commitOpTime->toBSON());
    }
    if (_abortOpTime) {
        builder->append("abortOpTime", _abortOpTime->toBSON());
    }
    _stats.report(builder);
}

void TenantMigrationDonorAccessBlocker::recordTenantMigrationError(Status status) {
    if (status == ErrorCodes::TenantMigrationConflict) {
        _stats.numBlockedWrites.addAndFetch(1);
    } else if (status == ErrorCodes::TenantMigrationCommitted) {
        _stats.numTenantMigrationCommittedErrors.addAndFetch(1);
    } else if (status == ErrorCodes::TenantMigrationAborted) {
        _stats.numTenantMigrationAbortedErrors.addAndFetch(1);
    }
}

void TenantMigrationDonorAccessBlocker::Stats::report(BSONObjBuilder* builder) const {
    builder->append("numBlockedReads", numBlockedReads.load());
    builder->append("numBlockedWrites", numBlockedWrites.load());
    builder->append("numTenantMigrationCommittedErrors", numTenantMigrationCommittedErrors.load());
    builder->append("numTenantMigrationAbortedErrors", numTenantMigrationAbortedErrors.load());
}

std::string TenantMigrationDonorAccessBlocker::BlockerState::toString(State state) {
    switch (state) {
        case State::kAllow:
            return "allow";
        case State::kBlockWrites:
            return "blockWrites";
        case State::kBlockWritesAndReads:
            return "blockWritesAndReads";
        case State::kReject:
            return "reject";
        case State::kAborted:
            return "aborted";
        default:
            MONGO_UNREACHABLE;
    }
}

bool TenantMigrationDonorAccessBlocker::BlockerState::_isLegalTransition(State oldState,
                                                                         State newState) {
    switch (oldState) {
        case State::kAllow:
            switch (newState) {
                case State::kBlockWrites:
                case State::kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case State::kBlockWrites:
            switch (newState) {
                case State::kAllow:
                case State::kBlockWritesAndReads:
                case State::kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case State::kBlockWritesAndReads:
            switch (newState) {
                case State::kAllow:
                case State::kReject:
                case State::kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case State::kReject:
            return false;
        case State::kAborted:
            return false;
    }
    MONGO_UNREACHABLE;
}

void TenantMigrationDonorAccessBlocker::BlockerState::transitionTo(State newState) {
    invariant(BlockerState::_isLegalTransition(_state, newState),
              str::stream() << "Current state: " << toString(_state)
                            << ", Illegal attempted next state: " << toString(newState));

    _state = newState;
}

}  // namespace mongo
