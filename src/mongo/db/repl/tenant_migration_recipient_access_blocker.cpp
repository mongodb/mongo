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


#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/logv2/log.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(tenantMigrationRecipientNotRejectReads);

}  // namespace

TenantMigrationRecipientAccessBlocker::TenantMigrationRecipientAccessBlocker(
    ServiceContext* serviceContext, const UUID& migrationId)
    : TenantMigrationAccessBlocker(BlockerType::kRecipient, migrationId),
      _serviceContext(serviceContext) {}

Status TenantMigrationRecipientAccessBlocker::checkIfCanWrite(Timestamp writeTs) {
    // This is guaranteed by the migration protocol. The recipient will not get any writes until the
    // migration is committed on the donor.
    return Status::OK();
}

Status TenantMigrationRecipientAccessBlocker::waitUntilCommittedOrAborted(OperationContext* opCtx) {
    // Recipient nodes will not throw TenantMigrationConflict errors and so we should never need
    // to wait for a migration to commit/abort on the recipient set.
    MONGO_UNREACHABLE;
}

SharedSemiFuture<void> TenantMigrationRecipientAccessBlocker::getCanReadFuture(
    OperationContext* opCtx, StringData command) {
    if (MONGO_unlikely(tenantMigrationRecipientNotRejectReads.shouldFail())) {
        return SharedSemiFuture<void>();
    }

    if (tenant_migration_access_blocker::shouldExcludeRead(opCtx)) {
        LOGV2_DEBUG(5739900,
                    1,
                    "Internal tenant read got excluded from the MTAB filtering",
                    "migrationId"_attr = getMigrationId(),
                    "opId"_attr = opCtx->getOpID());
        return SharedSemiFuture<void>();
    }

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
    if (_state.isReject()) {
        LOGV2_DEBUG(5749100,
                    1,
                    "Tenant read is blocked on the recipient before migration completes",
                    "migrationId"_attr = getMigrationId(),
                    "opId"_attr = opCtx->getOpID(),
                    "command"_attr = command);
        return SharedSemiFuture<void>(Status(
            ErrorCodes::SnapshotTooOld, "Tenant read is not allowed before migration completes"));
    }
    invariant(_state.isRejectBefore());
    invariant(_rejectBeforeTimestamp);
    if (atClusterTime && *atClusterTime < *_rejectBeforeTimestamp) {
        LOGV2_DEBUG(5749101,
                    1,
                    "Tenant read is blocked on the recipient before migration completes",
                    "migrationId"_attr = getMigrationId(),
                    "opId"_attr = opCtx->getOpID(),
                    "command"_attr = command,
                    "atClusterTime"_attr = *atClusterTime,
                    "rejectBeforeTimestamp"_attr = *_rejectBeforeTimestamp);
        return SharedSemiFuture<void>(Status(
            ErrorCodes::SnapshotTooOld, "Tenant read is not allowed before migration completes"));
    }
    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
        // Speculative majority reads are only used for change streams (against the oplog
        // collection) or when enableMajorityReadConcern=false. So we don't expect speculative
        // majority reads in serverless.

        auto executor = TenantMigrationAccessBlockerRegistry::get(_serviceContext)
                            .getAsyncBlockingOperationsExecutor();

        invariant(readConcernArgs.getMajorityReadMechanism() !=
                  repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative);
        return ExecutorFuture(executor)
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

Status TenantMigrationRecipientAccessBlocker::checkIfLinearizableReadWasAllowed(
    OperationContext* opCtx) {
    // The donor will block all writes at the blockOpTime, and will not signal the proxy to allow
    // reading from the recipient until that blockOpTime is majority committed on the recipient.
    // This means any writes made on the donor set are available in the majority snapshot of the
    // recipient, so linearizable guarantees will hold using the existing linearizable read
    // mechanism of doing a no-op write and waiting for it to be majority committed.
    return Status::OK();
}

Status TenantMigrationRecipientAccessBlocker::checkIfCanBuildIndex() {
    return Status::OK();
}

Status TenantMigrationRecipientAccessBlocker::checkIfCanOpenChangeStream() {
    return Status::OK();
}

Status TenantMigrationRecipientAccessBlocker::checkIfCanGetMoreChangeStream() {
    return Status::OK();
}

bool TenantMigrationRecipientAccessBlocker::checkIfShouldBlockTTL() const {
    stdx::lock_guard<Latch> lg(_mutex);
    return _ttlIsBlocked;
}

void TenantMigrationRecipientAccessBlocker::stopBlockingTTL() {
    stdx::lock_guard<Latch> lg(_mutex);
    _ttlIsBlocked = false;
}

void TenantMigrationRecipientAccessBlocker::onMajorityCommitPointUpdate(repl::OpTime opTime) {
    // Nothing to do.
    return;
}

void TenantMigrationRecipientAccessBlocker::appendInfoForServerStatus(
    BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_mutex);

    getMigrationId().appendToBuilder(builder, "migrationId");
    builder->append("state", _state.toString());
    if (_rejectBeforeTimestamp) {
        builder->append("rejectBeforeTimestamp", _rejectBeforeTimestamp.value());
    }
    builder->append("ttlIsBlocked", _ttlIsBlocked);
}

std::string TenantMigrationRecipientAccessBlocker::BlockerState::toString() const {
    switch (_state) {
        case State::kReject:
            return "reject";
        case State::kRejectBefore:
            return "rejectBefore";
        default:
            MONGO_UNREACHABLE;
    }
}

void TenantMigrationRecipientAccessBlocker::startRejectingReadsBefore(const Timestamp& timestamp) {
    stdx::lock_guard<Latch> lk(_mutex);
    _state.transitionToRejectBefore();
    if (!_rejectBeforeTimestamp || timestamp > *_rejectBeforeTimestamp) {
        LOGV2(5358100,
              "Tenant migration recipient starting to reject reads before timestamp",
              "migrationId"_attr = getMigrationId(),
              "timestamp"_attr = timestamp);
        _rejectBeforeTimestamp = timestamp;
    }
}

}  // namespace mongo
