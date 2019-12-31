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
#include "mongo/util/log.h"

namespace mongo {
namespace migrationutil {

MigrationCoordinator::MigrationCoordinator(OperationContext* opCtx,
                                           UUID migrationId,
                                           LogicalSessionId lsid,
                                           TxnNumber txnNumber,
                                           ShardId donorShard,
                                           ShardId recipientShard,
                                           NamespaceString collectionNamespace,
                                           UUID collectionUuid,
                                           ChunkRange range,
                                           ChunkVersion preMigrationChunkVersion)
    : _migrationInfo(migrationId,
                     std::move(lsid),
                     txnNumber,
                     std::move(collectionNamespace),
                     collectionUuid,
                     std::move(donorShard),
                     std::move(recipientShard),
                     std::move(range),
                     std::move(preMigrationChunkVersion)) {}

MigrationCoordinator::~MigrationCoordinator() = default;

void MigrationCoordinator::startMigration(OperationContext* opCtx, bool waitForDelete) {
    migrationutil::persistMigrationCoordinatorLocally(opCtx, _migrationInfo);

    RangeDeletionTask donorDeletionTask(_migrationInfo.getId(),
                                        _migrationInfo.getNss(),
                                        _migrationInfo.getCollectionUuid(),
                                        _migrationInfo.getDonorShardId(),
                                        _migrationInfo.getRange(),
                                        waitForDelete ? CleanWhenEnum::kNow
                                                      : CleanWhenEnum::kDelayed);
    donorDeletionTask.setPending(true);

    LOG(0) << "Persisting range deletion task on donor for migration " << _migrationInfo.getId();
    migrationutil::persistRangeDeletionTaskLocally(opCtx, donorDeletionTask);
}

void MigrationCoordinator::commitMigrationOnDonorAndRecipient(OperationContext* opCtx) {
    LOG(0) << "Committing migration on donor and recipient for migration "
           << _migrationInfo.getId();
    LOG(0) << "Deleting range deletion task on recipient for migration " << _migrationInfo.getId();

    migrationutil::deleteRangeDeletionTaskOnRecipient(opCtx,
                                                      _migrationInfo.getRecipientShardId(),
                                                      _migrationInfo.getId(),
                                                      _migrationInfo.getLsid(),
                                                      _migrationInfo.getTxnNumber());

    LOG(0) << "Marking range deletion task on donor as ready for processing for migration "
           << _migrationInfo.getId();
    migrationutil::markAsReadyRangeDeletionTaskLocally(opCtx, _migrationInfo.getId());
}

void MigrationCoordinator::abortMigrationOnDonorAndRecipient(OperationContext* opCtx) {
    LOG(0) << "Aborting migration on donor and recipient for migration " << _migrationInfo.getId();
    LOG(0) << "Deleting range deletion task on donor for migration " << _migrationInfo.getId();

    migrationutil::deleteRangeDeletionTaskLocally(opCtx, _migrationInfo.getId());

    LOG(0) << "Marking range deletion task on recipient as ready for processing for migration "
           << _migrationInfo.getId();

    migrationutil::markAsReadyRangeDeletionTaskOnRecipient(opCtx,
                                                           _migrationInfo.getRecipientShardId(),
                                                           _migrationInfo.getId(),
                                                           _migrationInfo.getLsid(),
                                                           _migrationInfo.getTxnNumber());
}

void MigrationCoordinator::forgetMigration(OperationContext* opCtx) {
    LOG(0) << "Deleting migration coordinator document for migration " << _migrationInfo.getId();
    migrationutil::deleteMigrationCoordinatorDocumentLocally(opCtx, _migrationInfo.getId());
}

}  // namespace migrationutil

}  // namespace mongo
