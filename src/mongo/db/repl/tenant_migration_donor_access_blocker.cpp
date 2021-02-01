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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_executor.h"
#include "mongo/db/repl/tenant_migration_committed_info.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/logv2/log.h"
#include "mongo/util/cancelation.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(tenantMigrationBlockRead);
MONGO_FAIL_POINT_DEFINE(tenantMigrationBlockWrite);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}  // namespace

TenantMigrationDonorAccessBlocker::TenantMigrationDonorAccessBlocker(
    ServiceContext* serviceContext, std::string tenantId, std::string recipientConnString)
    : _serviceContext(serviceContext),
      _tenantId(std::move(tenantId)),
      _recipientConnString(std::move(recipientConnString)) {
    _asyncBlockingOperationsExecutor = TenantMigrationAccessBlockerExecutor::get(serviceContext)
                                           .getOrCreateBlockedOperationsExecutor();
}

void TenantMigrationDonorAccessBlocker::checkIfCanWriteOrThrow() {
    stdx::lock_guard<Latch> lg(_mutex);

    switch (_state) {
        case State::kAllow:
            return;
        case State::kAborted:
            return;
        case State::kBlockWrites:
        case State::kBlockWritesAndReads:
            uasserted(TenantMigrationConflictInfo(_tenantId, shared_from_this()),
                      "Write must block until this tenant migration commits or aborts");
        case State::kReject:
            uasserted(TenantMigrationCommittedInfo(_tenantId, _recipientConnString),
                      "Write must be re-routed to the new owner of this tenant");
        default:
            MONGO_UNREACHABLE;
    }
}

Status TenantMigrationDonorAccessBlocker::waitUntilCommittedOrAborted(OperationContext* opCtx,
                                                                      OperationType operationType) {
    {
        stdx::unique_lock<Latch> ul(_mutex);

        auto canWrite = [&]() {
            return (operationType == kWrite && _state == State::kAllow) ||
                _state == State::kAborted;
        };

        if (!canWrite()) {
            tenantMigrationBlockWrite.shouldFail();  // Return value intentionally ignored.
        }
    }

    // Source to cancel the timeout if the operation completed in time.
    CancelationSource cancelTimeoutSource;
    auto executor = getAsyncBlockingOperationsExecutor();
    std::vector<ExecutorFuture<void>> futures;

    futures.emplace_back(_onCompletion().thenRunOn(executor));

    if (opCtx->hasDeadline()) {
        auto deadlineReachedFuture =
            executor->sleepUntil(opCtx->getDeadline(), cancelTimeoutSource.token());
        // The timeout condition is optional with index #1.
        futures.push_back(std::move(deadlineReachedFuture));
    }

    auto waitResult = whenAny(std::move(futures)).getNoThrow();
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
        return Status(opCtx->getTimeoutError(),
                      "Operation timed out waiting for tenant migration to commit or abort",
                      getDebugInfo());
    }
    MONGO_UNREACHABLE;
}

SharedSemiFuture<void> TenantMigrationDonorAccessBlocker::getCanReadFuture(
    OperationContext* opCtx) {
    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto readTimestamp = [opCtx, &readConcernArgs]() -> std::optional<Timestamp> {
        if (auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime()) {
            return afterClusterTime->asTimestamp();
        }
        if (auto atClusterTime = readConcernArgs.getArgsAtClusterTime()) {
            return atClusterTime->asTimestamp();
        }
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            return repl::StorageInterface::get(opCtx)->getPointInTimeReadTimestamp(opCtx);
        }
        return std::nullopt;
    }();
    if (!readTimestamp) {
        return SharedSemiFuture<void>();
    }
    return _getCanDoClusterTimeReadFuture(opCtx, *readTimestamp);
}

SharedSemiFuture<void> TenantMigrationDonorAccessBlocker::_getCanDoClusterTimeReadFuture(
    OperationContext* opCtx, Timestamp readTimestamp) {
    stdx::unique_lock<Latch> ul(_mutex);

    auto canRead = _state == State::kAllow || _state == State::kAborted ||
        _state == State::kBlockWrites || readTimestamp < *_blockTimestamp;

    if (!canRead) {
        tenantMigrationBlockRead.shouldFail();  // Return value intentionally ignored.
    }
    if (canRead) {
        return SharedSemiFuture<void>();
    }
    if (_state == State::kReject) {
        return SharedSemiFuture<void>(
            Status(ErrorCodes::TenantMigrationCommitted,
                   "Write or read must be re-routed to the new owner of this tenant",
                   TenantMigrationCommittedInfo(_tenantId, _recipientConnString).toBSON()));
    }

    return _transitionOutOfBlockingPromise.getFuture();
}

void TenantMigrationDonorAccessBlocker::checkIfLinearizableReadWasAllowedOrThrow(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lg(_mutex);
    uassert(TenantMigrationCommittedInfo(_tenantId, _recipientConnString),
            "Read must be re-routed to the new owner of this tenant",
            _state != State::kReject);
}

Status TenantMigrationDonorAccessBlocker::checkIfCanBuildIndex() {
    stdx::lock_guard<Latch> lg(_mutex);
    switch (_state) {
        case State::kAllow:
        case State::kBlockWrites:
        case State::kBlockWritesAndReads:
            return {TenantMigrationConflictInfo(_tenantId, shared_from_this(), kIndexBuild),
                    "Index build must block until tenant migration is committed or aborted."};
        case State::kReject:
            return {TenantMigrationCommittedInfo(_tenantId, _recipientConnString),
                    "Index build must be re-routed to the new owner of this tenant"};
        case State::kAborted:
            return Status::OK();
    }
    MONGO_UNREACHABLE;
}

void TenantMigrationDonorAccessBlocker::startBlockingWrites() {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(5093800, "Tenant migration starting to block writes", "tenantId"_attr = _tenantId);

    invariant(_state == State::kAllow);
    invariant(!_blockTimestamp);
    invariant(!_commitOpTime);
    invariant(!_abortOpTime);

    _state = State::kBlockWrites;
}

void TenantMigrationDonorAccessBlocker::startBlockingReadsAfter(const Timestamp& blockTimestamp) {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(5093801,
          "Tenant migration starting to block reads after blockTimestamp",
          "tenantId"_attr = _tenantId,
          "blockTimestamp"_attr = blockTimestamp);

    invariant(_state == State::kBlockWrites);
    invariant(!_blockTimestamp);
    invariant(!_commitOpTime);
    invariant(!_abortOpTime);

    _state = State::kBlockWritesAndReads;
    _blockTimestamp = blockTimestamp;
}

void TenantMigrationDonorAccessBlocker::rollBackStartBlocking() {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_state == State::kBlockWrites || _state == State::kBlockWritesAndReads);
    invariant(!_commitOpTime);
    invariant(!_abortOpTime);

    _state = State::kAllow;
    _blockTimestamp.reset();
    _transitionOutOfBlockingPromise.setFrom(Status::OK());
}

void TenantMigrationDonorAccessBlocker::setCommitOpTime(OperationContext* opCtx,
                                                        repl::OpTime opTime) {
    {
        stdx::lock_guard<Latch> lg(_mutex);

        invariant(_state == State::kBlockWritesAndReads);
        invariant(!_commitOpTime);
        invariant(!_abortOpTime);

        _commitOpTime = opTime;
    }

    LOGV2(5107300,
          "Tenant migration starting to wait for commit OpTime to be majority-committed",
          "tenantId"_attr = _tenantId,
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
          "Tenant migration starting to wait for abort OpTime to be majority-committed",
          "tenantId"_attr = _tenantId,
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
    invariant(_state == State::kBlockWritesAndReads);
    invariant(_blockTimestamp);
    invariant(_commitOpTime);
    invariant(!_abortOpTime);

    _state = State::kReject;
    Status error{ErrorCodes::TenantMigrationCommitted,
                 "Write or read must be re-routed to the new owner of this tenant",
                 TenantMigrationCommittedInfo(_tenantId, _recipientConnString).toBSON()};
    _completionPromise.setError(error);
    _transitionOutOfBlockingPromise.setFrom(error);

    lk.unlock();
    LOGV2(5093803,
          "Tenant migration finished waiting for commit OpTime to be majority-committed",
          "tenantId"_attr = _tenantId);
}

void TenantMigrationDonorAccessBlocker::_onMajorityCommitAbortOpTime(stdx::unique_lock<Latch>& lk) {
    invariant(_state != State::kReject);
    invariant(!_commitOpTime);
    invariant(_state != State::kAborted);
    invariant(_abortOpTime);

    _state = State::kAborted;
    _transitionOutOfBlockingPromise.setFrom(Status::OK());
    _completionPromise.setError({ErrorCodes::TenantMigrationAborted, "Tenant migration aborted"});

    lk.unlock();
    LOGV2(5093805,
          "Tenant migration finished waiting for abort OpTime to be majority-committed",
          "tenantId"_attr = _tenantId);
}

void TenantMigrationDonorAccessBlocker::appendInfoForServerStatus(BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(!_commitOpTime || !_abortOpTime);

    BSONObjBuilder tenantBuilder;
    tenantBuilder.append("state", _stateToString(_state));
    if (_blockTimestamp) {
        tenantBuilder.append("blockTimestamp", _blockTimestamp.get());
    }
    if (_commitOpTime) {
        tenantBuilder.append("commitOpTime", _commitOpTime->toBSON());
    }
    if (_abortOpTime) {
        tenantBuilder.append("abortOpTime", _abortOpTime->toBSON());
    }
    builder->append(_tenantId, tenantBuilder.obj());
}

std::string TenantMigrationDonorAccessBlocker::_stateToString(State state) const {
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

BSONObj TenantMigrationDonorAccessBlocker::getDebugInfo() const {
    return BSON("tenantId" << _tenantId << "recipientConnectionString" << _recipientConnString);
}

}  // namespace mongo
