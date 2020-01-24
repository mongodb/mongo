/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_coordinator.h"

#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(disableWritingPendingRangeDeletionEntries);

MONGO_FAIL_POINT_DEFINE(hangBeforeMakingCommitDecisionDurable);
MONGO_FAIL_POINT_DEFINE(hangBeforeMakingAbortDecisionDurable);

MONGO_FAIL_POINT_DEFINE(hangBeforeSendingCommitDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeSendingAbortDecision);

MONGO_FAIL_POINT_DEFINE(hangBeforeForgettingMigrationAfterCommitDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeForgettingMigrationAfterAbortDecision);

namespace migrationutil {

MigrationCoordinator::MigrationCoordinator(UUID migrationId,
                                           ShardId donorShard,
                                           ShardId recipientShard,
                                           NamespaceString collectionNamespace,
                                           UUID collectionUuid,
                                           ChunkRange range,
                                           ChunkVersion preMigrationChunkVersion)
    : _migrationInfo(migrationId,
                     std::move(collectionNamespace),
                     collectionUuid,
                     std::move(donorShard),
                     std::move(recipientShard),
                     std::move(range),
                     std::move(preMigrationChunkVersion)) {}

MigrationCoordinator::~MigrationCoordinator() = default;

void MigrationCoordinator::startMigration(OperationContext* opCtx, bool waitForDelete) {
    LOG(0) << _logPrefix() << "Persisting migration coordinator doc";
    migrationutil::persistMigrationCoordinatorLocally(opCtx, _migrationInfo);

    if (!disableWritingPendingRangeDeletionEntries.shouldFail()) {
        LOG(0) << _logPrefix() << "Persisting range deletion task on donor";
        RangeDeletionTask donorDeletionTask(_migrationInfo.getId(),
                                            _migrationInfo.getNss(),
                                            _migrationInfo.getCollectionUuid(),
                                            _migrationInfo.getDonorShardId(),
                                            _migrationInfo.getRange(),
                                            waitForDelete ? CleanWhenEnum::kNow
                                                          : CleanWhenEnum::kDelayed);
        donorDeletionTask.setPending(true);
        migrationutil::persistRangeDeletionTaskLocally(opCtx, donorDeletionTask);
    }
}

void MigrationCoordinator::setMigrationDecision(Decision decision) {
    LOG(0) << _logPrefix() << "MigrationCoordinator setting migration decision to "
           << (decision == Decision::kCommitted ? "committed" : "aborted");
    _decision = decision;
}


void MigrationCoordinator::completeMigration(OperationContext* opCtx) {
    if (!_decision) {
        LOG(0) << _logPrefix()
               << "Migration completed without setting a decision. This node might have started "
                  "stepping down or shutting down after having initiated commit against the config "
                  "server but before having found out if the commit succeeded. The new primary of "
                  "this replica set will complete the migration coordination.";
        return;
    }

    LOG(0) << _logPrefix() << "MigrationCoordinator delivering decision "
           << (_decision == Decision::kCommitted ? "committed" : "aborted")
           << " to self and to recipient";

    switch (*_decision) {
        case Decision::kAborted:
            _abortMigrationOnDonorAndRecipient(opCtx);
            hangBeforeForgettingMigrationAfterAbortDecision.pauseWhileSet();
            break;
        case Decision::kCommitted:
            _commitMigrationOnDonorAndRecipient(opCtx);
            hangBeforeForgettingMigrationAfterCommitDecision.pauseWhileSet();
            break;
    }
    forgetMigration(opCtx);
}

void MigrationCoordinator::_commitMigrationOnDonorAndRecipient(OperationContext* opCtx) {
    hangBeforeMakingCommitDecisionDurable.pauseWhileSet();

    LOG(0) << _logPrefix() << "Making commit decision durable";
    migrationutil::persistCommitDecision(opCtx, _migrationInfo.getId());

    hangBeforeSendingCommitDecision.pauseWhileSet();

    LOG(0) << _logPrefix() << "Deleting range deletion task on recipient";
    migrationutil::deleteRangeDeletionTaskOnRecipient(
        opCtx, _migrationInfo.getRecipientShardId(), _migrationInfo.getId());

    LOG(0) << _logPrefix() << "Marking range deletion task on donor as ready for processing";
    migrationutil::markAsReadyRangeDeletionTaskLocally(opCtx, _migrationInfo.getId());
}

void MigrationCoordinator::_abortMigrationOnDonorAndRecipient(OperationContext* opCtx) {
    hangBeforeMakingAbortDecisionDurable.pauseWhileSet();

    LOG(0) << _logPrefix() << "Making abort decision durable";
    migrationutil::persistAbortDecision(opCtx, _migrationInfo.getId());

    hangBeforeSendingAbortDecision.pauseWhileSet();

    LOG(0) << _logPrefix() << "Deleting range deletion task on donor";
    migrationutil::deleteRangeDeletionTaskLocally(opCtx, _migrationInfo.getId());

    LOG(0) << _logPrefix() << "Marking range deletion task on recipient as ready for processing";
    migrationutil::markAsReadyRangeDeletionTaskOnRecipient(
        opCtx, _migrationInfo.getRecipientShardId(), _migrationInfo.getId());
}

void MigrationCoordinator::forgetMigration(OperationContext* opCtx) {
    LOG(0) << _logPrefix() << "Deleting migration coordinator document";
    migrationutil::deleteMigrationCoordinatorDocumentLocally(opCtx, _migrationInfo.getId());
}

std::string MigrationCoordinator::_logPrefix() const {
    return str::stream() << "[migration-coordinator-" << _migrationInfo.getId() << "] ";
}

}  // namespace migrationutil

}  // namespace mongo
