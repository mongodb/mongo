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


#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_coordinator.h"

#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/pm2423_feature_flags_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeMakingCommitDecisionDurable);
MONGO_FAIL_POINT_DEFINE(hangBeforeMakingAbortDecisionDurable);

MONGO_FAIL_POINT_DEFINE(hangBeforeSendingCommitDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeSendingAbortDecision);

MONGO_FAIL_POINT_DEFINE(hangBeforeForgettingMigrationAfterCommitDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeForgettingMigrationAfterAbortDecision);

namespace {

LogicalSessionId getSystemLogicalSessionId() {
    static auto lsid = makeSystemLogicalSessionId();
    return lsid;
}

TxnNumber getNextTxnNumber() {
    static AtomicWord<TxnNumber> nextTxnNumber{0};
    return nextTxnNumber.fetchAndAdd(2);
}

}  // namespace

namespace migrationutil {

MigrationCoordinator::MigrationCoordinator(MigrationSessionId sessionId,
                                           ShardId donorShard,
                                           ShardId recipientShard,
                                           NamespaceString collectionNamespace,
                                           UUID collectionUuid,
                                           ChunkRange range,
                                           ChunkVersion preMigrationChunkVersion,
                                           bool waitForDelete)
    : _migrationInfo(UUID::gen(),
                     std::move(sessionId),
                     getSystemLogicalSessionId(),
                     getNextTxnNumber(),
                     std::move(collectionNamespace),
                     collectionUuid,
                     std::move(donorShard),
                     std::move(recipientShard),
                     std::move(range),
                     std::move(preMigrationChunkVersion)),
      _waitForDelete(waitForDelete) {}

MigrationCoordinator::MigrationCoordinator(const MigrationCoordinatorDocument& doc)
    : _migrationInfo(doc) {}

MigrationCoordinator::~MigrationCoordinator() = default;

const UUID& MigrationCoordinator::getMigrationId() const {
    return _migrationInfo.getId();
}

const LogicalSessionId& MigrationCoordinator::getLsid() const {
    return _migrationInfo.getLsid();
}

TxnNumber MigrationCoordinator::getTxnNumber() const {
    return _migrationInfo.getTxnNumber();
}

void MigrationCoordinator::startMigration(OperationContext* opCtx) {
    LOGV2_DEBUG(
        23889, 2, "Persisting migration coordinator doc", "migrationDoc"_attr = _migrationInfo);
    migrationutil::persistMigrationCoordinatorLocally(opCtx, _migrationInfo);

    LOGV2_DEBUG(23890,
                2,
                "Persisting range deletion task on donor",
                "migrationId"_attr = _migrationInfo.getId());
    RangeDeletionTask donorDeletionTask(_migrationInfo.getId(),
                                        _migrationInfo.getNss(),
                                        _migrationInfo.getCollectionUuid(),
                                        _migrationInfo.getDonorShardId(),
                                        _migrationInfo.getRange(),
                                        _waitForDelete ? CleanWhenEnum::kNow
                                                       : CleanWhenEnum::kDelayed);
    donorDeletionTask.setPending(true);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    donorDeletionTask.setTimestamp(currentTime.clusterTime().asTimestamp());
    migrationutil::persistRangeDeletionTaskLocally(
        opCtx, donorDeletionTask, WriteConcerns::kMajorityWriteConcernShardingTimeout);
}

void MigrationCoordinator::setMigrationDecision(DecisionEnum decision) {
    LOGV2_DEBUG(23891,
                2,
                "MigrationCoordinator setting migration decision to {decision}",
                "MigrationCoordinator setting migration decision",
                "decision"_attr = (decision == DecisionEnum::kCommitted ? "committed" : "aborted"),
                "migrationId"_attr = _migrationInfo.getId());
    _migrationInfo.setDecision(decision);
}


boost::optional<SemiFuture<void>> MigrationCoordinator::completeMigration(
    OperationContext* opCtx, bool acquireCSOnRecipient) {
    auto decision = _migrationInfo.getDecision();
    if (!decision) {
        LOGV2(
            23892,
            "Migration completed without setting a decision. This node might have "
            "started stepping down or shutting down after having initiated commit against the "
            "config server but before having found out if the commit succeeded. The new primary of "
            "this replica set will complete the migration coordination.",
            "migrationId"_attr = _migrationInfo.getId());
        return boost::none;
    }

    LOGV2(23893,
          "MigrationCoordinator delivering decision {decision} to self and to recipient",
          "MigrationCoordinator delivering decision to self and to recipient",
          "decision"_attr = (decision == DecisionEnum::kCommitted ? "committed" : "aborted"),
          "migrationId"_attr = _migrationInfo.getId());

    if (acquireCSOnRecipient) {
        if (!_releaseRecipientCriticalSectionFuture) {
            launchReleaseRecipientCriticalSection(opCtx);
        }
    }

    boost::optional<SemiFuture<void>> cleanupCompleteFuture = boost::none;

    switch (*decision) {
        case DecisionEnum::kAborted:
            _abortMigrationOnDonorAndRecipient(opCtx, acquireCSOnRecipient);
            hangBeforeForgettingMigrationAfterAbortDecision.pauseWhileSet();
            break;
        case DecisionEnum::kCommitted:
            cleanupCompleteFuture =
                _commitMigrationOnDonorAndRecipient(opCtx, acquireCSOnRecipient);
            hangBeforeForgettingMigrationAfterCommitDecision.pauseWhileSet();
            break;
    }

    forgetMigration(opCtx);

    return cleanupCompleteFuture;
}

SemiFuture<void> MigrationCoordinator::_commitMigrationOnDonorAndRecipient(
    OperationContext* opCtx, bool acquireCSOnRecipient) {
    hangBeforeMakingCommitDecisionDurable.pauseWhileSet();

    LOGV2_DEBUG(
        23894, 2, "Making commit decision durable", "migrationId"_attr = _migrationInfo.getId());
    migrationutil::persistCommitDecision(opCtx, _migrationInfo);

    if (acquireCSOnRecipient) {
        waitForReleaseRecipientCriticalSectionFuture(opCtx);
    }

    LOGV2_DEBUG(
        23895,
        2,
        "Bumping transaction number with lsid {lsid} and current txnNumber {currentTxnNumber} on "
        "recipient shard {recipientShardId} for commit of collection {nss}",
        "Bumping transaction number on recipient shard for commit",
        "namespace"_attr = _migrationInfo.getNss(),
        "recipientShardId"_attr = _migrationInfo.getRecipientShardId(),
        "lsid"_attr = _migrationInfo.getLsid(),
        "currentTxnNumber"_attr = _migrationInfo.getTxnNumber(),
        "migrationId"_attr = _migrationInfo.getId());
    migrationutil::advanceTransactionOnRecipient(opCtx,
                                                 _migrationInfo.getRecipientShardId(),
                                                 _migrationInfo.getLsid(),
                                                 _migrationInfo.getTxnNumber());

    hangBeforeSendingCommitDecision.pauseWhileSet();

    LOGV2_DEBUG(6376300,
                2,
                "Retrieving number of orphan documents from recipient",
                "migrationId"_attr = _migrationInfo.getId());

    const auto numOrphans = migrationutil::retrieveNumOrphansFromRecipient(opCtx, _migrationInfo);

    if (numOrphans > 0) {
        migrationutil::persistUpdatedNumOrphans(
            opCtx, _migrationInfo.getId(), _migrationInfo.getCollectionUuid(), numOrphans);
    }

    LOGV2_DEBUG(23896,
                2,
                "Deleting range deletion task on recipient",
                "migrationId"_attr = _migrationInfo.getId());
    migrationutil::deleteRangeDeletionTaskOnRecipient(
        opCtx, _migrationInfo.getRecipientShardId(), _migrationInfo.getId());

    LOGV2_DEBUG(23897,
                2,
                "Marking range deletion task on donor as ready for processing",
                "migrationId"_attr = _migrationInfo.getId());
    migrationutil::markAsReadyRangeDeletionTaskLocally(opCtx, _migrationInfo.getId());

    // At this point the decision cannot be changed and will be recovered in the event of a
    // failover, so it is safe to schedule the deletion task after updating the persisted state.
    LOGV2_DEBUG(23898,
                2,
                "Scheduling range deletion task on donor",
                "migrationId"_attr = _migrationInfo.getId());
    RangeDeletionTask deletionTask(_migrationInfo.getId(),
                                   _migrationInfo.getNss(),
                                   _migrationInfo.getCollectionUuid(),
                                   _migrationInfo.getDonorShardId(),
                                   _migrationInfo.getRange(),
                                   _waitForDelete ? CleanWhenEnum::kNow : CleanWhenEnum::kDelayed);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    deletionTask.setTimestamp(currentTime.clusterTime().asTimestamp());
    return migrationutil::submitRangeDeletionTask(opCtx, deletionTask).semi();
}

void MigrationCoordinator::_abortMigrationOnDonorAndRecipient(OperationContext* opCtx,
                                                              bool acquireCSOnRecipient) {
    hangBeforeMakingAbortDecisionDurable.pauseWhileSet();

    LOGV2_DEBUG(
        23899, 2, "Making abort decision durable", "migrationId"_attr = _migrationInfo.getId());
    migrationutil::persistAbortDecision(opCtx, _migrationInfo);

    hangBeforeSendingAbortDecision.pauseWhileSet();

    if (acquireCSOnRecipient) {
        waitForReleaseRecipientCriticalSectionFuture(opCtx);
    }

    // Ensure removing the local range deletion document to prevent incoming migrations with
    // overlapping ranges to hang.
    LOGV2_DEBUG(23901,
                2,
                "Deleting range deletion task on donor",
                "migrationId"_attr = _migrationInfo.getId());
    migrationutil::deleteRangeDeletionTaskLocally(opCtx, _migrationInfo.getId());

    try {
        LOGV2_DEBUG(23900,
                    2,
                    "Bumping transaction number with lsid {lsid} and current txnNumber "
                    "{currentTxnNumber} on "
                    "recipient shard {recipientShardId} for abort of collection {nss}",
                    "Bumping transaction number on recipient shard for abort",
                    "namespace"_attr = _migrationInfo.getNss(),
                    "recipientShardId"_attr = _migrationInfo.getRecipientShardId(),
                    "lsid"_attr = _migrationInfo.getLsid(),
                    "currentTxnNumber"_attr = _migrationInfo.getTxnNumber(),
                    "migrationId"_attr = _migrationInfo.getId());
        migrationutil::advanceTransactionOnRecipient(opCtx,
                                                     _migrationInfo.getRecipientShardId(),
                                                     _migrationInfo.getLsid(),
                                                     _migrationInfo.getTxnNumber());
    } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& exShardNotFound) {
        LOGV2_DEBUG(4620231,
                    1,
                    "Failed to advance transaction number on recipient shard for abort and/or "
                    "marking range deletion task on recipient as ready for processing",
                    "namespace"_attr = _migrationInfo.getNss(),
                    "migrationId"_attr = _migrationInfo.getId(),
                    "recipientShardId"_attr = _migrationInfo.getRecipientShardId(),
                    "currentTxnNumber"_attr = _migrationInfo.getTxnNumber(),
                    "error"_attr = exShardNotFound);
    }

    LOGV2_DEBUG(23902,
                2,
                "Marking range deletion task on recipient as ready for processing",
                "migrationId"_attr = _migrationInfo.getId());
    migrationutil::markAsReadyRangeDeletionTaskOnRecipient(
        opCtx, _migrationInfo.getRecipientShardId(), _migrationInfo.getId());
}

void MigrationCoordinator::forgetMigration(OperationContext* opCtx) {
    LOGV2_DEBUG(23903,
                2,
                "Deleting migration coordinator document",
                "migrationId"_attr = _migrationInfo.getId());
    migrationutil::deleteMigrationCoordinatorDocumentLocally(opCtx, _migrationInfo.getId());
}

void MigrationCoordinator::launchReleaseRecipientCriticalSection(OperationContext* opCtx) {
    _releaseRecipientCriticalSectionFuture =
        migrationutil::launchReleaseCriticalSectionOnRecipientFuture(
            opCtx,
            _migrationInfo.getRecipientShardId(),
            _migrationInfo.getNss(),
            _migrationInfo.getMigrationSessionId());
}

void MigrationCoordinator::waitForReleaseRecipientCriticalSectionFuture(OperationContext* opCtx) {
    invariant(_releaseRecipientCriticalSectionFuture);
    try {
        _releaseRecipientCriticalSectionFuture->get(opCtx);
    } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& exShardNotFound) {
        LOGV2(5899100,
              "Failed to releaseCriticalSectionOnRecipient",
              "shardId"_attr = _migrationInfo.getRecipientShardId(),
              "error"_attr = exShardNotFound);
    }
}

}  // namespace migrationutil

}  // namespace mongo
