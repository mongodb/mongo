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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_executor.h"
#include "mongo/db/repl/tenant_migration_committed_info.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/logv2/log.h"
#include "mongo/util/cancelation.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"

namespace mongo {

TenantMigrationRecipientAccessBlocker::TenantMigrationRecipientAccessBlocker(
    ServiceContext* serviceContext, std::string tenantId, std::string donorConnString)
    : _serviceContext(serviceContext),
      _tenantId(std::move(tenantId)),
      _donorConnString(std::move(donorConnString)) {
    _asyncBlockingOperationsExecutor = TenantMigrationAccessBlockerExecutor::get(serviceContext)
                                           .getOrCreateBlockedOperationsExecutor();
}

void TenantMigrationRecipientAccessBlocker::checkIfCanWriteOrThrow() {
    // This is guaranteed by the migration protocol. The recipient will not get any writes until the
    // migration is committed on the donor.
    return;
}

Status TenantMigrationRecipientAccessBlocker::waitUntilCommittedOrAborted(
    OperationContext* opCtx, OperationType operationType) {
    // Recipient nodes will not throw TenantMigrationConflict errors and so we should never need
    // to wait for a migration to commit/abort on the recipient set.
    MONGO_UNREACHABLE;
}

SharedSemiFuture<void> TenantMigrationRecipientAccessBlocker::getCanReadFuture(
    OperationContext* opCtx) {
    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto atClusterTime = [opCtx, &readConcernArgs]() -> boost::optional<Timestamp> {
        if (auto atClusterTime = readConcernArgs.getArgsAtClusterTime()) {
            return atClusterTime->asTimestamp();
        } else if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            return repl::StorageInterface::get(opCtx)->getPointInTimeReadTimestamp(opCtx);
        }
        return boost::none;
    }();

    stdx::lock_guard<Latch> lk(_mutex);
    if (_state == State::kReject) {
        return SharedSemiFuture<void>(Status(
            ErrorCodes::SnapshotTooOld, "Tenant read is not allowed before migration completes"));
    }
    invariant(_state == State::kRejectBefore);
    invariant(_rejectBeforeTimestamp);
    if (atClusterTime && *atClusterTime < *_rejectBeforeTimestamp) {
        return SharedSemiFuture<void>(Status(
            ErrorCodes::SnapshotTooOld, "Tenant read is not allowed before migration completes"));
    }
    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
        // Speculative majority reads are only used for change streams (against the oplog
        // collection) or when enableMajorityReadConcern=false. So we don't expect speculative
        // majority reads in serverless.
        invariant(readConcernArgs.getMajorityReadMechanism() !=
                  repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative);
        return ExecutorFuture(_asyncBlockingOperationsExecutor)
            .then([timestamp = *_rejectBeforeTimestamp, deadline = opCtx->getDeadline()] {
                auto uniqueOpCtx = cc().makeOperationContext();
                auto opCtx = uniqueOpCtx.get();
                opCtx->setDeadlineByDate(deadline, ErrorCodes::MaxTimeMSExpired);
                repl::ReplicationCoordinator::get(opCtx)->waitUntilSnapshotCommitted(opCtx,
                                                                                     timestamp);
            })
            .share();
    }
    return SharedSemiFuture<void>();
}

void TenantMigrationRecipientAccessBlocker::checkIfLinearizableReadWasAllowedOrThrow(
    OperationContext* opCtx) {
    // The donor will block all writes at the blockOpTime, and will not signal the proxy to allow
    // reading from the recipient until that blockOpTime is majority committed on the recipient.
    // This means any writes made on the donor set are available in the majority snapshot of the
    // recipient, so linearizable guarantees will hold using the existing linearizable read
    // mechanism of doing a no-op write and waiting for it to be majority committed.
    return;
}

Status TenantMigrationRecipientAccessBlocker::checkIfCanBuildIndex() {
    return Status::OK();
}

void TenantMigrationRecipientAccessBlocker::onMajorityCommitPointUpdate(repl::OpTime opTime) {
    // Nothing to do.
    return;
}

void TenantMigrationRecipientAccessBlocker::appendInfoForServerStatus(
    BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_mutex);

    BSONObjBuilder tenantBuilder;
    tenantBuilder.append("state", _stateToString(_state));
    if (_rejectBeforeTimestamp) {
        tenantBuilder.append("rejectBeforeTimestamp", _rejectBeforeTimestamp.get());
    }
    builder->append(_tenantId, tenantBuilder.obj());
}

std::string TenantMigrationRecipientAccessBlocker::_stateToString(State state) const {
    switch (state) {
        case State::kReject:
            return "reject";
        case State::kRejectBefore:
            return "rejectBefore";
        default:
            MONGO_UNREACHABLE;
    }
}

BSONObj TenantMigrationRecipientAccessBlocker::getDebugInfo() const {
    return BSON("tenantId" << _tenantId << "donorConnectionString" << _donorConnString);
}

void TenantMigrationRecipientAccessBlocker::startRejectingReadsBefore(const Timestamp& timestamp) {
    stdx::lock_guard<Latch> lk(_mutex);
    _state = State::kRejectBefore;
    if (!_rejectBeforeTimestamp || timestamp > *_rejectBeforeTimestamp) {
        LOGV2(5358100,
              "Tenant migration recipient starting to reject reads before timestamp",
              "tenantId"_attr = _tenantId,
              "timestamp"_attr = timestamp);
        _rejectBeforeTimestamp = timestamp;
    }
}

}  // namespace mongo
