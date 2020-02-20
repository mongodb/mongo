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
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeMakingCommitDecisionDurable);
MONGO_FAIL_POINT_DEFINE(hangBeforeMakingAbortDecisionDurable);

MONGO_FAIL_POINT_DEFINE(hangBeforeSendingCommitDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeSendingAbortDecision);

MONGO_FAIL_POINT_DEFINE(hangBeforeForgettingMigrationAfterCommitDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeForgettingMigrationAfterAbortDecision);

namespace migrationutil {

MigrationCoordinator::MigrationCoordinator(UUID migrationId,
                                           MigrationSessionId sessionId,
                                           LogicalSessionId lsid,
                                           ShardId donorShard,
                                           ShardId recipientShard,
                                           NamespaceString collectionNamespace,
                                           UUID collectionUuid,
                                           ChunkRange range,
                                           ChunkVersion preMigrationChunkVersion,
                                           bool waitForDelete)
    : _migrationInfo(migrationId,
                     std::move(sessionId),
                     std::move(lsid),
                     std::move(collectionNamespace),
                     collectionUuid,
                     std::move(donorShard),
                     std::move(recipientShard),
                     std::move(range),
                     std::move(preMigrationChunkVersion)),
      _waitForDelete(waitForDelete) {}

MigrationCoordinator::~MigrationCoordinator() = default;

void MigrationCoordinator::startMigration(OperationContext* opCtx) {
    LOGV2(
        23889, "{logPrefix}Persisting migration coordinator doc", "logPrefix"_attr = _logPrefix());
    migrationutil::persistMigrationCoordinatorLocally(opCtx, _migrationInfo);

    LOGV2(23890,
          "{logPrefix}Persisting range deletion task on donor",
          "logPrefix"_attr = _logPrefix());
    RangeDeletionTask donorDeletionTask(_migrationInfo.getId(),
                                        _migrationInfo.getNss(),
                                        _migrationInfo.getCollectionUuid(),
                                        _migrationInfo.getDonorShardId(),
                                        _migrationInfo.getRange(),
                                        _waitForDelete ? CleanWhenEnum::kNow
                                                       : CleanWhenEnum::kDelayed);
    donorDeletionTask.setPending(true);
    migrationutil::persistRangeDeletionTaskLocally(opCtx, donorDeletionTask);
}

void MigrationCoordinator::setMigrationDecision(Decision decision) {
    LOGV2(23891,
          "{logPrefix}MigrationCoordinator setting migration decision to "
          "{decision_Decision_kCommitted_committed_aborted}",
          "logPrefix"_attr = _logPrefix(),
          "decision_Decision_kCommitted_committed_aborted"_attr =
              (decision == Decision::kCommitted ? "committed" : "aborted"));
    _decision = decision;
}


boost::optional<SemiFuture<void>> MigrationCoordinator::completeMigration(OperationContext* opCtx) {
    if (!_decision) {
        LOGV2(23892,
              "{logPrefix}Migration completed without setting a decision. This node might have "
              "started "
              "stepping down or shutting down after having initiated commit against the config "
              "server but before having found out if the commit succeeded. The new primary of "
              "this replica set will complete the migration coordination.",
              "logPrefix"_attr = _logPrefix());
        return boost::none;
    }

    LOGV2(23893,
          "{logPrefix}MigrationCoordinator delivering decision "
          "{decision_Decision_kCommitted_committed_aborted} to self and to recipient",
          "logPrefix"_attr = _logPrefix(),
          "decision_Decision_kCommitted_committed_aborted"_attr =
              (_decision == Decision::kCommitted ? "committed" : "aborted"));

    boost::optional<SemiFuture<void>> cleanupCompleteFuture = boost::none;

    switch (*_decision) {
        case Decision::kAborted:
            _abortMigrationOnDonorAndRecipient(opCtx);
            hangBeforeForgettingMigrationAfterAbortDecision.pauseWhileSet();
            break;
        case Decision::kCommitted:
            cleanupCompleteFuture = _commitMigrationOnDonorAndRecipient(opCtx);
            hangBeforeForgettingMigrationAfterCommitDecision.pauseWhileSet();
            break;
    }

    forgetMigration(opCtx);

    return cleanupCompleteFuture;
}

SemiFuture<void> MigrationCoordinator::_commitMigrationOnDonorAndRecipient(
    OperationContext* opCtx) {
    hangBeforeMakingCommitDecisionDurable.pauseWhileSet();

    LOGV2(23894, "{logPrefix}Making commit decision durable", "logPrefix"_attr = _logPrefix());
    migrationutil::persistCommitDecision(opCtx, _migrationInfo.getId());

    LOGV2(23895,
          "{logPrefix}Bumping transaction for {migrationInfo_getRecipientShardId} lsid: "
          "{migrationInfo_getLsid} txn: {TxnNumber_1}",
          "logPrefix"_attr = _logPrefix(),
          "migrationInfo_getRecipientShardId"_attr = _migrationInfo.getRecipientShardId(),
          "migrationInfo_getLsid"_attr = _migrationInfo.getLsid().toBSON(),
          "TxnNumber_1"_attr = TxnNumber{1});
    migrationutil::advanceTransactionOnRecipient(
        opCtx, _migrationInfo.getRecipientShardId(), _migrationInfo.getLsid(), TxnNumber{1});

    hangBeforeSendingCommitDecision.pauseWhileSet();

    LOGV2(23896,
          "{logPrefix}Deleting range deletion task on recipient",
          "logPrefix"_attr = _logPrefix());
    migrationutil::deleteRangeDeletionTaskOnRecipient(
        opCtx, _migrationInfo.getRecipientShardId(), _migrationInfo.getId());

    LOGV2(23897,
          "{logPrefix}Marking range deletion task on donor as ready for processing",
          "logPrefix"_attr = _logPrefix());
    migrationutil::markAsReadyRangeDeletionTaskLocally(opCtx, _migrationInfo.getId());

    // At this point the decision cannot be changed and will be recovered in the event of a
    // failover, so it is safe to schedule the deletion task after updating the persisted state.
    LOGV2(23898,
          "{logPrefix}Scheduling range deletion task on donor",
          "logPrefix"_attr = _logPrefix());
    RangeDeletionTask deletionTask(_migrationInfo.getId(),
                                   _migrationInfo.getNss(),
                                   _migrationInfo.getCollectionUuid(),
                                   _migrationInfo.getDonorShardId(),
                                   _migrationInfo.getRange(),
                                   _waitForDelete ? CleanWhenEnum::kNow : CleanWhenEnum::kDelayed);
    return migrationutil::submitRangeDeletionTask(opCtx, deletionTask).semi();
}

void MigrationCoordinator::_abortMigrationOnDonorAndRecipient(OperationContext* opCtx) {
    hangBeforeMakingAbortDecisionDurable.pauseWhileSet();

    LOGV2(23899, "{logPrefix}Making abort decision durable", "logPrefix"_attr = _logPrefix());
    migrationutil::persistAbortDecision(opCtx, _migrationInfo.getId());

    LOGV2(23900,
          "{logPrefix}Bumping transaction for {migrationInfo_getRecipientShardId} lsid: "
          "{migrationInfo_getLsid} txn: {TxnNumber_1}",
          "logPrefix"_attr = _logPrefix(),
          "migrationInfo_getRecipientShardId"_attr = _migrationInfo.getRecipientShardId(),
          "migrationInfo_getLsid"_attr = _migrationInfo.getLsid().toBSON(),
          "TxnNumber_1"_attr = TxnNumber{1});
    migrationutil::advanceTransactionOnRecipient(
        opCtx, _migrationInfo.getRecipientShardId(), _migrationInfo.getLsid(), TxnNumber{1});

    hangBeforeSendingAbortDecision.pauseWhileSet();

    LOGV2(
        23901, "{logPrefix}Deleting range deletion task on donor", "logPrefix"_attr = _logPrefix());
    migrationutil::deleteRangeDeletionTaskLocally(opCtx, _migrationInfo.getId());

    LOGV2(23902,
          "{logPrefix}Marking range deletion task on recipient as ready for processing",
          "logPrefix"_attr = _logPrefix());
    migrationutil::markAsReadyRangeDeletionTaskOnRecipient(
        opCtx, _migrationInfo.getRecipientShardId(), _migrationInfo.getId());
}

void MigrationCoordinator::forgetMigration(OperationContext* opCtx) {
    LOGV2(23903,
          "{logPrefix}Deleting migration coordinator document",
          "logPrefix"_attr = _logPrefix());
    migrationutil::deleteMigrationCoordinatorDocumentLocally(opCtx, _migrationInfo.getId());
}

std::string MigrationCoordinator::_logPrefix() const {
    return str::stream() << "[migration-coordinator-" << _migrationInfo.getId() << "] ";
}

}  // namespace migrationutil

}  // namespace mongo
