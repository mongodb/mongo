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
#include "mongo/db/repl/migrating_tenant_access_blocker.h"
#include "mongo/db/repl/replication_coordinator.h"

namespace mongo {

MigratingTenantAccessBlocker::MigratingTenantAccessBlocker(ServiceContext* serviceContext,
                                                           executor::TaskExecutor* executor)
    : _serviceContext(serviceContext), _executor(executor) {}

void MigratingTenantAccessBlocker::checkIfCanWriteOrThrow() {
    stdx::lock_guard<Latch> lg(_mutex);

    switch (_access) {
        case Access::kAllow:
            return;
        case Access::kBlockWrites:
        case Access::kBlockWritesAndReads:
            uasserted(ErrorCodes::TenantMigrationConflict,
                      "Write must block until this tenant migration commits or aborts");
        case Access::kReject:
            uasserted(ErrorCodes::TenantMigrationCommitted,
                      "Write must be re-routed to the new owner of this tenant");
        default:
            MONGO_UNREACHABLE;
    }
}

void MigratingTenantAccessBlocker::checkIfCanWriteOrBlock(OperationContext* opCtx) {
    stdx::unique_lock<Latch> ul(_mutex);

    opCtx->waitForConditionOrInterrupt(_transitionOutOfBlockingCV, ul, [&]() {
        return _access == Access::kAllow || _access == Access::kReject;
    });

    uassert(ErrorCodes::TenantMigrationCommitted,
            "Write must be re-routed to the new owner of this database",
            _access == Access::kAllow);
}

void MigratingTenantAccessBlocker::checkIfCanDoClusterTimeReadOrBlock(
    OperationContext* opCtx, const Timestamp& readTimestamp) {
    stdx::unique_lock<Latch> ul(_mutex);

    auto canRead = [&]() {
        return _access == Access::kAllow || _access == Access::kBlockWrites ||
            readTimestamp < *_blockTimestamp;
    };

    opCtx->waitForConditionOrInterrupt(
        _transitionOutOfBlockingCV, ul, [&]() { return canRead() || _access == Access::kReject; });

    uassert(ErrorCodes::TenantMigrationCommitted,
            "Read must be re-routed to the new owner of this database",
            canRead());
}

void MigratingTenantAccessBlocker::checkIfLinearizableReadWasAllowedOrThrow(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lg(_mutex);
    uassert(ErrorCodes::TenantMigrationCommitted,
            "Read must be re-routed to the new owner of this database",
            _access != Access::kReject);
}

void MigratingTenantAccessBlocker::startBlockingWrites() {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kAllow);
    invariant(!_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kBlockWrites;
}

void MigratingTenantAccessBlocker::startBlockingReadsAfter(const Timestamp& blockTimestamp) {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kBlockWrites);
    invariant(!_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kBlockWritesAndReads;
    _blockTimestamp = blockTimestamp;
}

void MigratingTenantAccessBlocker::rollBackStartBlocking() {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kBlockWrites || _access == Access::kBlockWritesAndReads);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kAllow;
    _blockTimestamp.reset();
    _transitionOutOfBlockingCV.notify_all();
}

void MigratingTenantAccessBlocker::commit(repl::OpTime commitOpTime) {
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
    };

    _waitForOpTimeToMajorityCommit(commitOpTime, callbackFn);
}

void MigratingTenantAccessBlocker::abort(repl::OpTime abortOpTime) {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_access == Access::kBlockWritesAndReads);
    invariant(_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _commitOrAbortOpTime = abortOpTime;

    const auto callbackFn = [this, abortOpTime]() {
        stdx::lock_guard<Latch> lg(_mutex);

        invariant(_access == Access::kBlockWritesAndReads);
        invariant(_blockTimestamp);
        invariant(_commitOrAbortOpTime == abortOpTime);
        invariant(_waitForCommitOrAbortToMajorityCommitOpCtx);

        _access = Access::kAllow;
        _blockTimestamp.reset();
        _commitOrAbortOpTime.reset();
        _waitForCommitOrAbortToMajorityCommitOpCtx = nullptr;
        _transitionOutOfBlockingCV.notify_all();
    };

    _waitForOpTimeToMajorityCommit(abortOpTime, callbackFn);
}

void MigratingTenantAccessBlocker::rollBackCommitOrAbort() {
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

void MigratingTenantAccessBlocker::_waitForOpTimeToMajorityCommit(
    repl::OpTime opTime, std::function<void()> callbackFn) {
    uassertStatusOK(
        _executor->scheduleWork([&](const executor::TaskExecutor::CallbackArgs& cbData) {
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

                ThreadClient tc("MigratingTenantAccessBlocker::commit", _serviceContext);
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

                try {
                    repl::ReplicationCoordinator::get(opCtx)->waitUntilSnapshotCommitted(
                        opCtx, opTime.getTimestamp());
                    status = Status::OK();
                } catch (const DBException& ex) {
                    status = ex.toStatus();
                }
            }

            // 'opTime' became majority committed.
            callbackFn();
        }));
}

void MigratingTenantAccessBlocker::appendInfoForServerStatus(BSONObjBuilder* builder) const {
    builder->append("access", _access);
    if (_blockTimestamp) {
        builder->append("blockTimestamp", _blockTimestamp.get());
    }

    if (_commitOrAbortOpTime) {
        builder->append("commitOrAbortOpTime", _commitOrAbortOpTime->toBSON());
    }
}

}  // namespace mongo
