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
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker.h"
#include "mongo/db/repl/tenant_migration_committed_info.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(tenantMigrationBlockRead);
MONGO_FAIL_POINT_DEFINE(tenantMigrationBlockWrite);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}  // namespace

void TenantMigrationAccessBlocker::checkIfCanWriteOrThrow() {
    stdx::lock_guard<Latch> lg(_mutex);

    switch (_access) {
        case Access::kAllow:
            return;
        case Access::kBlockWrites:
        case Access::kBlockWritesAndReads:
            uasserted(TenantMigrationConflictInfo(_tenantId),
                      "Write must block until this tenant migration commits or aborts");
        case Access::kReject:
            uasserted(TenantMigrationCommittedInfo(_tenantId, _recipientConnString),
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
        uasserted(TenantMigrationCommittedInfo(_tenantId, _recipientConnString),
                  "Write must be re-routed to the new owner of this tenant");
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

    uassert(TenantMigrationCommittedInfo(_tenantId, _recipientConnString),
            "Read must be re-routed to the new owner of this tenant",
            canRead());
}

void TenantMigrationAccessBlocker::checkIfLinearizableReadWasAllowedOrThrow(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lg(_mutex);
    uassert(TenantMigrationCommittedInfo(_tenantId, _recipientConnString),
            "Read must be re-routed to the new owner of this tenant",
            _access != Access::kReject);
}

void TenantMigrationAccessBlocker::startBlockingWrites() {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(5093800, "Tenant migration starting to block writes", "tenantId"_attr = _tenantId);

    invariant(!_inShutdown);
    invariant(_access == Access::kAllow);
    invariant(!_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kBlockWrites;
}

void TenantMigrationAccessBlocker::startBlockingReadsAfter(const Timestamp& blockTimestamp) {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(5093801,
          "Tenant migration starting to block reads after blockTimestamp",
          "tenantId"_attr = _tenantId,
          "blockTimestamp"_attr = blockTimestamp);

    invariant(!_inShutdown);
    invariant(_access == Access::kBlockWrites);
    invariant(!_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kBlockWritesAndReads;
    _blockTimestamp = blockTimestamp;
}

void TenantMigrationAccessBlocker::rollBackStartBlocking() {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(!_inShutdown);
    invariant(_access == Access::kBlockWrites || _access == Access::kBlockWritesAndReads);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _access = Access::kAllow;
    _blockTimestamp.reset();
    _transitionOutOfBlockingCV.notify_all();
}

void TenantMigrationAccessBlocker::commit(repl::OpTime commitOpTime) {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(5093802,
          "Tenant migration starting to wait for commit OpTime to be majority-committed",
          "tenantId"_attr = _tenantId,
          "commitOpTime"_attr = commitOpTime);

    invariant(!_inShutdown);
    invariant(_access == Access::kBlockWritesAndReads);
    invariant(_blockTimestamp);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _commitOrAbortOpTime = commitOpTime;

    _waitForOpTimeToMajorityCommit(commitOpTime)
        .then([this, self = shared_from_this(), commitOpTime]() {
            stdx::lock_guard<Latch> lg(_mutex);

            invariant(_access == Access::kBlockWritesAndReads);
            invariant(_blockTimestamp);
            invariant(_commitOrAbortOpTime == commitOpTime);
            invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

            _access = Access::kReject;
            _transitionOutOfBlockingCV.notify_all();
            _completionPromise.emplaceValue();
        })
        .getAsync([this, self = shared_from_this()](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            LOGV2(5093803,
                  "Tenant migration finished waiting for commit OpTime to be majority-committed",
                  "tenantId"_attr = _tenantId,
                  "status"_attr = status);
        });
}

void TenantMigrationAccessBlocker::abort(repl::OpTime abortOpTime) {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(5093804,
          "Tenant migration starting to wait for abort OpTime to be majority-committed",
          "tenantId"_attr = _tenantId,
          "abortOpTime"_attr = abortOpTime);

    invariant(!_inShutdown);
    invariant(!_commitOrAbortOpTime);
    invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

    _commitOrAbortOpTime = abortOpTime;

    _waitForOpTimeToMajorityCommit(abortOpTime)
        .then([this, self = shared_from_this(), abortOpTime]() {
            stdx::lock_guard<Latch> lg(_mutex);

            invariant(_commitOrAbortOpTime == abortOpTime);
            invariant(!_waitForCommitOrAbortToMajorityCommitOpCtx);

            _access = Access::kAllow;
            _transitionOutOfBlockingCV.notify_all();
            _completionPromise.setError(
                {ErrorCodes::TenantMigrationAborted, "tenant migration aborted"});
        })
        .getAsync([this, self = shared_from_this()](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            LOGV2(5093805,
                  "Tenant migration finished waiting for abort OpTime to be majority-committed",
                  "tenantId"_attr = _tenantId,
                  "status"_attr = status);
        });
}

void TenantMigrationAccessBlocker::shutDown() {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_inShutdown) {
        return;
    }

    _inShutdown = true;
    if (_waitForCommitOrAbortToMajorityCommitOpCtx) {
        stdx::lock_guard<Client> lk(*_waitForCommitOrAbortToMajorityCommitOpCtx->getClient());
        _waitForCommitOrAbortToMajorityCommitOpCtx->markKilled();
    }
}

SharedSemiFuture<void> TenantMigrationAccessBlocker::onCompletion() {
    return _completionPromise.getFuture();
}

ExecutorFuture<void> TenantMigrationAccessBlocker::_waitForOpTimeToMajorityCommit(
    repl::OpTime opTime) {
    return AsyncTry([this, self = shared_from_this(), opTime] {
               ThreadClient tc("TenantMigrationAccessBlocker", _serviceContext);
               const auto opCtxHolder = tc->makeOperationContext();
               const auto opCtx = opCtxHolder.get();

               // We will save 'opCtx' below, so make sure we clear it before 'opCtx' is destroyed.
               const auto guard = makeGuard([&] {
                   stdx::lock_guard<Latch> lg(_mutex);
                   _waitForCommitOrAbortToMajorityCommitOpCtx = nullptr;
               });

               {
                   stdx::lock_guard<Latch> lg(_mutex);

                   uassert(ErrorCodes::TenantMigrationAccessBlockerShuttingDown,
                           "TenantMigrationAccessBlocker was shut down",
                           !_inShutdown);

                   // Save 'opCtx' so that if shutDown() is called after this point, 'opCtx' will be
                   // interrupted and 'waitUntilMajorityOpTime' will return an interrupt error.
                   _waitForCommitOrAbortToMajorityCommitOpCtx = opCtx;
               }
               uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->waitUntilMajorityOpTime(
                   opCtx, opTime));
           })
        .until([this, self = shared_from_this(), opTime](Status status) {
            bool shouldStop =
                status.isOK() || status == ErrorCodes::TenantMigrationAccessBlockerShuttingDown;
            if (!shouldStop) {
                LOGV2(5093806,
                      "Tenant migration retrying waiting for OpTime to be majority-committed",
                      "tenantId"_attr = _tenantId,
                      "opTime"_attr = opTime,
                      "status"_attr = status);
            }
            return shouldStop;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(_executor);
}

void TenantMigrationAccessBlocker::appendInfoForServerStatus(BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_mutex);

    BSONObjBuilder tenantBuilder;
    tenantBuilder.append("access", _access);
    if (_blockTimestamp) {
        tenantBuilder.append("blockTimestamp", _blockTimestamp.get());
    }
    if (_commitOrAbortOpTime) {
        tenantBuilder.append("commitOrAbortOpTime", _commitOrAbortOpTime->toBSON());
    }
    builder->append(_tenantId, tenantBuilder.obj());
}

}  // namespace mongo
