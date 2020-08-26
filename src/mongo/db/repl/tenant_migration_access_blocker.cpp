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
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(tenantMigrationBlockRead);
MONGO_FAIL_POINT_DEFINE(tenantMigrationBlockWrite);

}  // namespace

TenantMigrationAccessBlocker::TenantMigrationAccessBlocker(
    ServiceContext* serviceContext,
    std::unique_ptr<executor::TaskExecutor> executor,
    std::string dbPrefix)
    : _serviceContext(serviceContext),
      _executor(std::move(executor)),
      _dbPrefix(std::move(dbPrefix)) {
    _executor->startup();
}

void TenantMigrationAccessBlocker::checkIfCanWriteOrThrow() {
    stdx::lock_guard<Latch> lg(_mutex);

    switch (_access) {
        case Access::kAllow:
            return;
        case Access::kBlockWrites:
        case Access::kBlockWritesAndReads:
            uasserted(TenantMigrationConflictInfo(_dbPrefix),
                      "Write must block until this tenant migration commits or aborts");
        case Access::kReject:
            uasserted(ErrorCodes::TenantMigrationCommitted,
                      "Write must be re-routed to the new owner of this tenant");
        default:
            MONGO_UNREACHABLE;
    }
}

void TenantMigrationAccessBlocker::checkIfCanWriteOrBlock(OperationContext* opCtx) {
    stdx::unique_lock<Latch> ul(_mutex);

    auto canWrite = [&]() { return _access == Access::kAllow; };

    if (!canWrite()) {
        tenantMigrationBlockWrite.shouldFail();
    }

    opCtx->waitForConditionOrInterrupt(
        _transitionOutOfBlockingCV, ul, [&]() { return canWrite() || _access == Access::kReject; });

    auto status = onCompletion().getNoThrow();
    if (status.isOK()) {
        invariant(_access == Access::kReject);
        uasserted(ErrorCodes::TenantMigrationCommitted,
                  "Write must be re-routed to the new owner of this database");
    }
    uassertStatusOK(status);
}

void TenantMigrationAccessBlocker::checkIfCanDoClusterTimeReadOrBlock(
    OperationContext* opCtx, const Timestamp& readTimestamp) {
    stdx::unique_lock<Latch> ul(_mutex);

    auto canRead = [&]() {
        return _access == Access::kAllow || _access == Access::kBlockWrites ||
            readTimestamp < *_blockTimestamp;
    };

    if (!canRead()) {
        tenantMigrationBlockRead.shouldFail();
    }

    opCtx->waitForConditionOrInterrupt(
        _transitionOutOfBlockingCV, ul, [&]() { return canRead() || _access == Access::kReject; });

    uassert(ErrorCodes::TenantMigrationCommitted,
            "Read must be re-routed to the new owner of this database",
            canRead());
}

void TenantMigrationAccessBlocker::checkIfLinearizableReadWasAllowedOrThrow(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lg(_mutex);
    uassert(ErrorCodes::TenantMigrationCommitted,
            "Read must be re-routed to the new owner of this database",
            _access != Access::kReject);
}

void TenantMigrationAccessBlocker::startBlockingWrites() {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kAllow);
    invariant(!_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kBlockWrites;
}

void TenantMigrationAccessBlocker::startBlockingReadsAfter(const Timestamp& blockTimestamp) {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kBlockWrites);
    invariant(!_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kBlockWritesAndReads;
    _blockTimestamp = blockTimestamp;
}

void TenantMigrationAccessBlocker::rollBackStartBlocking() {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kBlockWrites || _access == Access::kBlockWritesAndReads);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kAllow;
    _blockTimestamp.reset();
    _transitionOutOfBlockingCV.notify_all();
}

void TenantMigrationAccessBlocker::commit(repl::OpTime commitOpTime) {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kBlockWritesAndReads);
    invariant(_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _commitOrAbortOpTime = commitOpTime;

    const auto callbackFn = [this, commitOpTime]() {
        stdx::lock_guard<Latch> lg(_mutex);

        invariant(_access == Access::kBlockWritesAndReads);
        invariant(_blockTimestamp);
        invariant(_commitOrAbortOpTime == commitOpTime);
        invariant(_waitForCommitOrAbortToMajorityCommitOpCtx);

        _access = Access::kReject;
        _transitionOutOfBlockingCV.notify_all();
        _completionPromise.emplaceValue();
    };

    _waitForOpTimeToMajorityCommit(commitOpTime, callbackFn);
}

void TenantMigrationAccessBlocker::abort(repl::OpTime abortOpTime) {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _commitOrAbortOpTime = abortOpTime;

    const auto callbackFn = [this, abortOpTime]() {
        stdx::lock_guard<Latch> lg(_mutex);

        invariant(_commitOrAbortOpTime == abortOpTime);
        invariant(_waitForCommitOrAbortToMajorityCommitOpCtx);

        _access = Access::kAllow;
        _blockTimestamp.reset();
        _commitOrAbortOpTime.reset();
        _waitForCommitOrAbortToMajorityCommitOpCtx = nullptr;
        _transitionOutOfBlockingCV.notify_all();
        _completionPromise.setError(
            {ErrorCodes::TenantMigrationAborted, "tenant migration aborted"});
    };

    _waitForOpTimeToMajorityCommit(abortOpTime, callbackFn);
}

void TenantMigrationAccessBlocker::rollBackCommitOrAbort() {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kBlockWritesAndReads);
    invariant(_blockTimestamp);
    invariant(_commitOrAbortOpTime);

    _commitOrAbortOpTime.reset();
    if (_waitForCommitOrAbortToMajorityCommitOpCtx) {
        stdx::lock_guard<Client> lk(*_waitForCommitOrAbortToMajorityCommitOpCtx->getClient());
        _waitForCommitOrAbortToMajorityCommitOpCtx->markKilled();
    }
    _waitForCommitOrAbortToMajorityCommitOpCtx = nullptr;
}

SharedSemiFuture<void> TenantMigrationAccessBlocker::onCompletion() {
    return _completionPromise.getFuture();
}

void TenantMigrationAccessBlocker::_waitForOpTimeToMajorityCommit(
    repl::OpTime opTime, std::function<void()> callbackFn) {
    uassertStatusOK(
        _executor->scheduleWork([=](const executor::TaskExecutor::CallbackArgs& cbData) {
            if (!cbData.status.isOK()) {
                return;
            }

            // Keep waiting for 'opTime' to become majority committed until the node is shutting
            // down or 'opTime' in particular has rolled back.
            Status status(ErrorCodes::InternalError, "Not Set");
            while (!status.isOK()) {
                if (ErrorCodes::isA<ErrorCategory::ShutdownError>(status.code())) {
                    return;
                }

                ThreadClient tc("TenantMigrationAccessBlocker::commit", _serviceContext);
                const auto opCtxHolder = tc->makeOperationContext();
                const auto opCtx = opCtxHolder.get();

                {
                    stdx::lock_guard<Latch> lg(_mutex);

                    // Check if 'opTime' has rolled back.
                    if (!_commitOrAbortOpTime || _commitOrAbortOpTime != opTime) {
                        return;
                    }

                    // Save 'opCtx' so that if 'opTime' rolls back after this point, 'opCtx' will be
                    // interrupted and 'waitUntilSnapshotCommitted' will return an interrupt error.
                    _waitForCommitOrAbortToMajorityCommitOpCtx = opCtx;
                }

                status = repl::ReplicationCoordinator::get(opCtx)->waitUntilMajorityOpTime(opCtx,
                                                                                           opTime);
            }

            // 'opTime' became majority committed.
            callbackFn();
        }));
}

void TenantMigrationAccessBlocker::appendInfoForServerStatus(BSONObjBuilder* builder) const {
    BSONObjBuilder tenantBuilder;
    tenantBuilder.append("access", _access);
    if (_blockTimestamp) {
        tenantBuilder.append("blockTimestamp", _blockTimestamp.get());
    }
    if (_commitOrAbortOpTime) {
        tenantBuilder.append("commitOrAbortOpTime", _commitOrAbortOpTime->toBSON());
    }
    builder->append(_dbPrefix, tenantBuilder.obj());
}

}  // namespace mongo
