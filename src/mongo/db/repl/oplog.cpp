/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog.h"

#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/capped_collection_maintenance.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/health_log_gen.h"
#include "mongo/db/catalog/health_log_interface.h"
#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/catalog/index_build_oplog_entry.h"
#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/client.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_index.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/resumable_index_builds_gen.h"
#include "mongo/db/s/sharding_index_catalog_ddl_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/file.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {

using namespace std::string_literals;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace repl {
namespace {

// Failpoint to block after a write and its oplog entry have been written to the storage engine and
// are visible, but before we have advanced 'lastApplied' for the write.
MONGO_FAIL_POINT_DEFINE(hangBeforeLogOpAdvancesLastApplied);

void abortIndexBuilds(OperationContext* opCtx,
                      const OplogEntry::CommandType& commandType,
                      const NamespaceString& nss,
                      const std::string& reason) {
    auto indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
    if (commandType == OplogEntry::CommandType::kDropDatabase) {
        indexBuildsCoordinator->abortDatabaseIndexBuilds(opCtx, nss.dbName(), reason);
    } else if (commandType == OplogEntry::CommandType::kDrop ||
               commandType == OplogEntry::CommandType::kDropIndexes ||
               commandType == OplogEntry::CommandType::kCollMod ||
               commandType == OplogEntry::CommandType::kEmptyCapped ||
               commandType == OplogEntry::CommandType::kRenameCollection) {
        const boost::optional<UUID> collUUID =
            CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
        invariant(collUUID);

        indexBuildsCoordinator->abortCollectionIndexBuilds(opCtx, nss, *collUUID, reason);
    }
}

void applyImportCollectionDefault(OperationContext* opCtx,
                                  const UUID& importUUID,
                                  const NamespaceString& nss,
                                  long long numRecords,
                                  long long dataSize,
                                  const BSONObj& catalogEntry,
                                  const BSONObj& storageMetadata,
                                  bool isDryRun,
                                  OplogApplication::Mode mode) {
    LOGV2_FATAL_NOTRACE(5114200,
                        "Applying importCollection is not supported with MongoDB Community "
                        "Edition, please use MongoDB Enterprise Edition",
                        "importUUID"_attr = importUUID,
                        logAttrs(nss),
                        "numRecords"_attr = numRecords,
                        "dataSize"_attr = dataSize,
                        "catalogEntry"_attr = redact(catalogEntry),
                        "storageMetadata"_attr = redact(storageMetadata),
                        "isDryRun"_attr = isDryRun);
}

StringData getInvalidatingReason(const OplogApplication::Mode mode, const bool isDataConsistent) {
    if (mode == OplogApplication::Mode::kInitialSync) {
        return "initial sync"_sd;
    } else if (!isDataConsistent) {
        return "minvalid suggests inconsistent snapshot"_sd;
    }

    return ""_sd;
}

Status insertDocumentsForOplog(OperationContext* opCtx,
                               const CollectionPtr& oplogCollection,
                               std::vector<Record>* records,
                               const std::vector<Timestamp>& timestamps) {
    invariant(shard_role_details::getLocker(opCtx)->isWriteLocked());

    Status status = oplogCollection->getRecordStore()->insertRecords(opCtx, records, timestamps);
    if (!status.isOK())
        return status;

    collection_internal::cappedDeleteUntilBelowConfiguredMaximum(
        opCtx, oplogCollection, records->begin()->id);

    // We do not need to notify capped waiters, as we have not yet updated oplog visibility, so
    // these inserts will not be visible.  When visibility updates, it will notify capped
    // waiters.
    return Status::OK();
}

void assertInitialSyncCanContinueDuringShardMerge(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const OplogEntry& op) {
    // Running shard merge during initial sync can lead to potential data loss on this node.
    // So, we perform safety check during oplog catchup and at the end of initial sync
    // recovery. (see recoverShardMergeRecipientAccessBlockers() for the detailed comment about the
    // problematic scenario that can cause data loss.)
    if (nss == NamespaceString::kShardMergeRecipientsNamespace) {
        if (auto replCoord = repl::ReplicationCoordinator::get(opCtx); replCoord &&
            replCoord->getSettings().isReplSet() && replCoord->getMemberState().startup2()) {
            BSONElement idField = op.getObject().getField("_id");
            // If the 'o' field does not have an _id, then 'o2' should have it.
            // Otherwise, the oplog entry is corrupted.
            if (idField.eoo() && op.getObject2()) {
                idField = op.getObject2()->getField("_id");
            }
            const auto& migrationId = uassertStatusOK(UUID::parse(idField));
            tenant_migration_access_blocker::assertOnUnsafeInitialSync(migrationId);
        }
    }
}

}  // namespace

ApplyImportCollectionFn applyImportCollection = applyImportCollectionDefault;

void registerApplyImportCollectionFn(ApplyImportCollectionFn func) {
    applyImportCollection = func;
}

void createIndexForApplyOps(OperationContext* opCtx,
                            const BSONObj& indexSpec,
                            const NamespaceString& indexNss,
                            OplogApplication::Mode mode) {
    // Uncommitted collections support creating indexes using relaxed locking if they are part of a
    // multi-document transaction.
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(indexNss, MODE_X) ||
              (UncommittedCatalogUpdates::get(opCtx).isCreatedCollection(opCtx, indexNss) &&
               shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(indexNss, MODE_IX) &&
               opCtx->inMultiDocumentTransaction()));

    // Check if collection exists.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, indexNss.dbName());
    auto indexCollection = CollectionPtr(
        db ? CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, indexNss) : nullptr);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to create index due to missing collection: "
                          << indexNss.toStringForErrorMsg(),
            indexCollection);

    OpCounters* opCounters = opCtx->writesAreReplicated() ? &globalOpCounters : &replOpCounters;
    opCounters->gotInsert();
    if (opCtx->writesAreReplicated()) {
        ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
            opCtx->getWriteConcern());
    }

    // Check for conflict with two-phase index builds during initial sync. It is possible that
    // this index may have been dropped and recreated after inserting documents into the collection.
    if (OplogApplication::Mode::kInitialSync == mode) {
        const auto normalSpec =
            IndexCatalog::normalizeIndexSpecs(opCtx, indexCollection, indexSpec);
        auto indexCatalog = indexCollection->getIndexCatalog();
        auto prepareSpecResult =
            indexCatalog->prepareSpecForCreate(opCtx, indexCollection, normalSpec, {});
        if (ErrorCodes::IndexBuildAlreadyInProgress == prepareSpecResult) {
            LOGV2(4924900,
                  "Index build: already in progress during initial sync",
                  logAttrs(indexNss),
                  "uuid"_attr = indexCollection->uuid(),
                  "spec"_attr = indexSpec);
            return;
        }
    }

    // This function should not be used outside oplog application. We should be able to always set
    // the index build constraints to kRelax.
    invariant(ReplicationCoordinator::get(opCtx)->shouldRelaxIndexConstraints(opCtx, indexNss),
              str::stream() << "Unexpected result from shouldRelaxIndexConstraints - ns: "
                            << indexNss.toStringForErrorMsg() << "; uuid: "
                            << indexCollection->uuid() << "; original index spec: " << indexSpec);
    const auto constraints = IndexBuildsManager::IndexConstraints::kRelax;

    // Run single-phase builds synchronously with oplog batch application. For tenant migrations,
    // the recipient needs to build the index on empty collections to completion within the same
    // storage transaction. This is in order to eliminate a window of time where we can reload the
    // catalog through startup or rollback and detect the index in an incomplete state. Before
    // SERVER-72618 this was possible and would require us to remove the index from the catalog to
    // allow replication recovery to rebuild it. The result of this was an untimestamped write to
    // the catalog. This only applies to empty collection index builds during tenant migration and
    // is resolved by calling `createIndexesOnEmptyCollection` on empty collections.
    //
    // Single phase builds are only used for empty collections, and to rebuild indexes admin.system
    // collections. See SERVER-47439.
    IndexBuildsCoordinator::updateCurOpOpDescription(opCtx, indexNss, {indexSpec});
    auto collUUID = indexCollection->uuid();
    auto fromMigrate = false;
    auto indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
    if (indexCollection->isEmpty(opCtx)) {
        WriteUnitOfWork wuow(opCtx);
        CollectionWriter coll(opCtx, indexNss);

        try {
            indexBuildsCoordinator->createIndexesOnEmptyCollection(
                opCtx, coll, {indexSpec}, fromMigrate);
        } catch (DBException& ex) {
            // Some indexing errors can be ignored during oplog application.
            const auto& status = ex.toStatus();
            if (IndexBuildsCoordinator::isCreateIndexesErrorSafeToIgnore(status, constraints)) {
                LOGV2_DEBUG(7261800,
                            1,
                            "Ignoring indexing error",
                            "error"_attr = redact(status),
                            logAttrs(indexCollection->ns()),
                            logAttrs(indexCollection->uuid()),
                            "spec"_attr = indexSpec);
                return;
            }
            throw;
        }
        wuow.commit();
    } else {
        indexBuildsCoordinator->createIndex(opCtx, collUUID, indexSpec, constraints, fromMigrate);
    }
}

/**
 * @param dataImage can be BSONObj::isEmpty to signal the node is in initial sync and must
 *                  invalidate relevant image collection data.
 */
void writeToImageCollection(OperationContext* opCtx,
                            const LogicalSessionId& sessionId,
                            const TxnNumber txnNum,
                            const Timestamp timestamp,
                            repl::RetryImageEnum imageKind,
                            const BSONObj& dataImage,
                            const StringData& invalidatedReason) {
    // In practice, this lock acquisition on kConfigImagesNamespace cannot block. The only time a
    // stronger lock acquisition is taken on this namespace is during step up to create the
    // collection.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
        shard_role_details::getLocker(opCtx));
    auto collection = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);
    repl::ImageEntry imageEntry;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(txnNum);
    imageEntry.setTs(timestamp);
    imageEntry.setImageKind(imageKind);
    imageEntry.setImage(dataImage);
    if (dataImage.isEmpty()) {
        imageEntry.setInvalidated(true);
        imageEntry.setInvalidatedReason(invalidatedReason);
    }

    DisableDocumentValidation documentValidationDisabler(
        opCtx, DocumentValidationSettings::kDisableInternalValidation);

    UpdateRequest request;
    request.setNamespaceString(NamespaceString::kConfigImagesNamespace);
    request.setQuery(BSON("_id" << imageEntry.get_id().toBSON()));
    request.setUpsert(true);
    request.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(imageEntry.toBSON()));
    request.setFromOplogApplication(true);
    // This code path can also be hit by things such as `applyOps` and tenant migrations.
    ::mongo::update(opCtx, collection, request);
}

/* we write to local.oplog.rs:
     { ts : ..., v: ..., op: ..., etc }
   ts: an OpTime timestamp
   v: version
   op:
    "i" insert
    "u" update
    "d" delete
    "c" db cmd
    "n" no op
    "xi" insert global index key
    "xd" delete global index key
*/


void logOplogRecords(OperationContext* opCtx,
                     const NamespaceString& nss,
                     std::vector<Record>* records,
                     const std::vector<Timestamp>& timestamps,
                     const CollectionPtr& oplogCollection,
                     OpTime finalOpTime,
                     Date_t wallTime,
                     bool isAbortIndexBuild) {
    auto replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && !replCoord->canAcceptWritesFor(opCtx, nss)) {
        str::stream ss;
        ss << "logOp() but can't accept write to collection " << nss.toStringForErrorMsg();
        ss << ": entries: " << records->size() << ": [ ";
        for (const auto& record : *records) {
            ss << "(" << record.id << ", " << redact(record.data.toBson()) << ") ";
        }
        ss << "]";
        uasserted(ErrorCodes::NotWritablePrimary, ss);
    }

    // Throw TenantMigrationConflict error if the database for 'nss' is being migrated. The oplog
    // entry for renameCollection has 'nss' set to the fromCollection's ns. renameCollection can be
    // across databases, but a tenant will never be able to rename into a database with a different
    // prefix, so it is safe to use the fromCollection's db's prefix for this check.
    //
    // Skip the check if this is an "abortIndexBuild" oplog entry since it is safe to the abort an
    // index build on the donor after the blockTimestamp, plus if an index build fails to commit due
    // to TenantMigrationConflict, we need to be able to abort the index build and clean up.
    if (!isAbortIndexBuild) {
        tenant_migration_access_blocker::checkIfCanWriteOrThrow(
            opCtx, nss.dbName(), timestamps.back());
    }

    Status result = insertDocumentsForOplog(opCtx, oplogCollection, records, timestamps);
    if (!result.isOK()) {
        LOGV2_FATAL(17322, "Write to oplog failed", "error"_attr = result.toString());
    }

    // Insert the oplog records to the respective tenants change collections.
    if (change_stream_serverless_helpers::isChangeCollectionsModeActive()) {
        ChangeStreamChangeCollectionManager::get(opCtx).insertDocumentsToChangeCollection(
            opCtx, *records, timestamps);
    }

    // Set replCoord last optime only after we're sure the WUOW didn't abort and roll back.
    shard_role_details::getRecoveryUnit(opCtx)->onCommit([replCoord, finalOpTime, wallTime](
                                                             OperationContext* opCtx,
                                                             boost::optional<Timestamp>
                                                                 commitTime) {
        if (commitTime) {
            // The `finalOpTime` may be less than the `commitTime` if multiple oplog entries
            // are logging within one WriteUnitOfWork.
            invariant(finalOpTime.getTimestamp() <= *commitTime,
                      str::stream() << "Final OpTime: " << finalOpTime.toString()
                                    << ". Commit Time: " << commitTime->toString());
        }

        // Optionally hang before advancing lastApplied.
        if (MONGO_unlikely(hangBeforeLogOpAdvancesLastApplied.shouldFail())) {
            LOGV2(21243, "hangBeforeLogOpAdvancesLastApplied fail point enabled");
            hangBeforeLogOpAdvancesLastApplied.pauseWhileSet(opCtx);
        }

        // Optimes on the primary should always represent consistent database states.
        replCoord->setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward({finalOpTime, wallTime});

        // We set the last op on the client to 'finalOpTime', because that contains the
        // timestamp of the operation that the client actually performed.
        ReplClientInfo::forClient(opCtx->getClient()).setLastOp(opCtx, finalOpTime);
    });
}

OpTime logOp(OperationContext* opCtx, MutableOplogEntry* oplogEntry) {
    addDestinedRecipient.execute([&](const BSONObj& data) {
        auto recipient = data["destinedRecipient"].String();
        oplogEntry->setDestinedRecipient(boost::make_optional<ShardId>({recipient}));
    });
    // All collections should have UUIDs now, so all insert, update, and delete oplog entries should
    // also have uuids. Some no-op (n) and command (c) entries may still elide the uuid field.
    invariant(oplogEntry->getUuid() || oplogEntry->getOpType() == OpTypeEnum::kNoop ||
                  oplogEntry->getOpType() == OpTypeEnum::kCommand,
              str::stream() << "Expected uuid for logOp with oplog entry: "
                            << redact(oplogEntry->toBSON()));

    auto replCoord = ReplicationCoordinator::get(opCtx);
    // For commands, the test below is on the command ns and therefore does not check for
    // specific namespaces such as system.profile. This is the caller's responsibility.
    if (replCoord->isOplogDisabledFor(opCtx, oplogEntry->getNss())) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "retryable writes is not supported for unreplicated ns: "
                              << oplogEntry->getNss().toStringForErrorMsg(),
                oplogEntry->getStatementIds().empty());
        return {};
    }
    // If this oplog entry is from a tenant migration, include the tenant migration
    // UUID and optional donor timeline metadata.
    if (const auto& recipientInfo = tenantMigrationInfo(opCtx)) {
        oplogEntry->setFromTenantMigration(recipientInfo->uuid);
        if (oplogEntry->getTid() &&
            change_stream_serverless_helpers::isChangeStreamEnabled(opCtx, *oplogEntry->getTid()) &&
            recipientInfo->donorOplogEntryData) {
            oplogEntry->setDonorOpTime(recipientInfo->donorOplogEntryData->donorOpTime);
            oplogEntry->setDonorApplyOpsIndex(recipientInfo->donorOplogEntryData->applyOpsIndex);
        }
    }


    // TODO SERVER-51301 to remove this block.
    if (oplogEntry->getOpType() == repl::OpTypeEnum::kNoop) {
        shard_role_details::getRecoveryUnit(opCtx)->ignoreAllMultiTimestampConstraints();
    }

    // Use OplogAccessMode::kLogOp to avoid recursive locking.
    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kLogOp);
    auto oplogInfo = oplogWrite.getOplogInfo();

    // If an OpTime is not specified (i.e. isNull), a new OpTime will be assigned to the oplog entry
    // within the WUOW. If a new OpTime is assigned, it needs to be reset back to a null OpTime
    // before exiting this function so that the same oplog entry instance can be reused for logOp()
    // again. For example, if the WUOW gets aborted within a writeConflictRetry loop, we need to
    // reset the OpTime to null so a new OpTime will be assigned on retry.
    OplogSlot slot = oplogEntry->getOpTime();
    ScopeGuard resetOpTimeGuard([&, resetOpTimeOnExit = bool(slot.isNull())] {
        if (resetOpTimeOnExit)
            oplogEntry->setOpTime(OplogSlot());
    });

    WriteUnitOfWork wuow(opCtx);
    if (slot.isNull()) {
        slot = oplogInfo->getNextOpTimes(opCtx, 1U)[0];
        // It would be better to make the oplogEntry a const reference. But because in some cases, a
        // new OpTime needs to be assigned within the WUOW as explained earlier, we instead pass
        // oplogEntry by pointer and reset the OpTime to null using a ScopeGuard.
        oplogEntry->setOpTime(slot);
    }

    const auto& oplog = oplogInfo->getCollection();
    auto wallClockTime = oplogEntry->getWallClockTime();

    auto bsonOplogEntry = oplogEntry->toBSON();
    // The storage engine will assign the RecordId based on the "ts" field of the oplog entry, see
    // record_id_helpers::extractKey.
    std::vector<Record> records{
        {RecordId(), RecordData(bsonOplogEntry.objdata(), bsonOplogEntry.objsize())}};
    std::vector<Timestamp> timestamps{slot.getTimestamp()};
    const auto isAbortIndexBuild = oplogEntry->getOpType() == OpTypeEnum::kCommand &&
        parseCommandType(oplogEntry->getObject()) == OplogEntry::CommandType::kAbortIndexBuild;
    logOplogRecords(opCtx,
                    oplogEntry->getNss(),
                    &records,
                    timestamps,
                    CollectionPtr(oplog),
                    slot,
                    wallClockTime,
                    isAbortIndexBuild);
    wuow.commit();
    return slot;
}

void appendOplogEntryChainInfo(OperationContext* opCtx,
                               MutableOplogEntry* oplogEntry,
                               OplogLink* oplogLink,
                               const std::vector<StmtId>& stmtIds) {
    invariant(!stmtIds.empty());

    // Not a retryable write.
    if (stmtIds.front() == kUninitializedStmtId) {
        // If the statement id is uninitialized, it must be the only one. There cannot also be
        // initialized statement ids.
        invariant(stmtIds.size() == 1);
        return;
    }

    const auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);
    oplogEntry->setSessionId(opCtx->getLogicalSessionId());
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());
    // If this is a multi-operation retryable oplog entry, the statement IDs should not be included
    // because they were included in the individual operations.
    if (oplogLink->multiOpType != MultiOplogEntryType::kLegacyMultiOpType) {
        oplogEntry->setMultiOpType(oplogLink->multiOpType);
    } else {
        oplogEntry->setStatementIds(stmtIds);
    }
    if (oplogLink->prevOpTime.isNull()) {
        oplogLink->prevOpTime = txnParticipant.getLastWriteOpTime();
    }
    oplogEntry->setPrevWriteOpTimeInTransaction(oplogLink->prevOpTime);
}

namespace {
long long getNewOplogSizeBytes(OperationContext* opCtx, const ReplSettings& replSettings) {
    if (replSettings.getOplogSizeBytes() != 0) {
        return replSettings.getOplogSizeBytes();
    }
    /* not specified. pick a default size */
    ProcessInfo pi;
    if (pi.getAddrSize() == 32) {
        const auto sz = 50LL * 1024LL * 1024LL;
        LOGV2_DEBUG(21245, 3, "Choosing oplog size for 32bit system", "oplogSizeBytes"_attr = sz);
        return sz;
    }
    // First choose a minimum size.

#if defined(__APPLE__)
    // typically these are desktops (dev machines), so keep it smallish
    const auto sz = 192 * 1024 * 1024;
    LOGV2_DEBUG(21246, 3, "Choosing oplog size for Apple system", "oplogSizeBytes"_attr = sz);
    return sz;
#else
    long long lowerBound = 0;
    double bytes = 0;
    if (opCtx->getClient()->getServiceContext()->getStorageEngine()->isEphemeral()) {
        // in memory: 50MB minimum size
        lowerBound = 50LL * 1024 * 1024;
        bytes = pi.getMemSizeMB() * 1024 * 1024;
        LOGV2_DEBUG(21247,
                    3,
                    "Ephemeral storage system",
                    "lowerBoundBytes"_attr = lowerBound,
                    "totalMemoryBytes"_attr = bytes);
    } else {
        // disk: 990MB minimum size
        lowerBound = 990LL * 1024 * 1024;
        bytes = File::freeSpace(storageGlobalParams.dbpath);  //-1 if call not supported.
        LOGV2_DEBUG(21248,
                    3,
                    "Disk storage system",
                    "lowerBoundBytes"_attr = lowerBound,
                    "freeSpaceBytes"_attr = bytes);
    }
    long long fivePct = static_cast<long long>(bytes * 0.05);
    auto sz = std::max(fivePct, lowerBound);

    // Round up oplog size to nearest 256 alignment. Ensures that the rollback of the 256-alignment
    // requirement for capped collections that was removed in SERVER-67246 will not impact this
    // important user-hidden collection. Since downgrades are blocked on this alignment check, a
    // fresh install of the server would very likely lead to an inability to downgrade. Keeping the
    // oplog size 256-aligned avoids this altogether.
    long long sz_prior = sz;
    sz += 0xff;
    sz &= 0xffffffffffffff00LL;
    if (sz_prior != sz) {
        LOGV2(7421400,
              "Oplog size is being rounded to nearest 256-byte-aligned size",
              "oldSize"_attr = sz_prior,
              "newSize"_attr = sz);
    }

    // we use 5% of free [disk] space up to 50GB (1TB free)
    const long long upperBound = 50LL * 1024 * 1024 * 1024;
    sz = std::min(sz, upperBound);
    return sz;
#endif
}

}  // namespace

void createOplog(OperationContext* opCtx,
                 const NamespaceString& oplogCollectionName,
                 bool isReplSet) {
    Lock::GlobalWrite lk(opCtx);

    const auto service = opCtx->getServiceContext();

    const ReplSettings& replSettings = ReplicationCoordinator::get(opCtx)->getSettings();

    OldClientContext ctx(opCtx, oplogCollectionName);
    const Collection* collection =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, oplogCollectionName);

    if (collection) {
        if (replSettings.getOplogSizeBytes() != 0) {
            const CollectionOptions& oplogOpts = collection->getCollectionOptions();

            int o = (int)(oplogOpts.cappedSize / (1024 * 1024));
            int n = (int)(replSettings.getOplogSizeBytes() / (1024 * 1024));
            if (n != o) {
                static constexpr char message[] =
                    "Command line oplog size different than existing. See "
                    "http://dochub.mongodb.org/core/increase-oplog";
                LOGV2(
                    21249, message, "commandLineOplogSize"_attr = n, "existingOplogSize"_attr = o);
                uasserted(13257,
                          str::stream() << message << ". Command line oplog size: " << n
                                        << ", existing oplog size: " << o);
            }
        }
        acquireOplogCollectionForLogging(opCtx);
        if (!isReplSet)
            initTimestampFromOplog(opCtx, oplogCollectionName);
        return;
    }

    /* create an oplog collection, if it doesn't yet exist. */
    const auto sz = getNewOplogSizeBytes(opCtx, replSettings);

    LOGV2(21251, "Creating replication oplog", "oplogSizeMB"_attr = (int)(sz / (1024 * 1024)));

    CollectionOptions options;
    options.capped = true;
    options.cappedSize = sz;
    options.autoIndexId = CollectionOptions::NO;

    writeConflictRetry(opCtx, "createCollection", oplogCollectionName, [&] {
        WriteUnitOfWork uow(opCtx);
        invariant(ctx.db()->createCollection(opCtx, oplogCollectionName, options));
        acquireOplogCollectionForLogging(opCtx);
        if (!isReplSet) {
            service->getOpObserver()->onOpMessage(opCtx, BSONObj());
        }
        uow.commit();
    });

    /* sync here so we don't get any surprising lag later when we try to sync */
    service->getStorageEngine()->flushAllFiles(opCtx, /*callerHoldsReadLock*/ false);
}

void createOplog(OperationContext* opCtx) {
    const auto isReplSet = ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();
    createOplog(opCtx, NamespaceString::kRsOplogNamespace, isReplSet);
}

std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count) {
    return LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, count);
}

// -------------------------------------

namespace {
NamespaceString extractNs(DatabaseName dbName, const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    uassert(40073,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    StringData coll = first.valueStringData();
    uassert(28635, "no collection name specified", !coll.empty());
    return NamespaceStringUtil::deserialize(dbName, coll);
}

NamespaceString extractNsFromUUID(OperationContext* opCtx, const UUID& uuid) {
    auto catalog = CollectionCatalog::get(opCtx);
    auto nss = catalog->lookupNSSByUUID(opCtx, uuid);
    uassert(ErrorCodes::NamespaceNotFound, "No namespace with UUID " + uuid.toString(), nss);
    return *nss;
}

NamespaceString extractNsFromUUIDorNs(OperationContext* opCtx,
                                      const NamespaceString& ns,
                                      const boost::optional<UUID>& ui,
                                      const BSONObj& cmd) {
    return ui ? extractNsFromUUID(opCtx, ui.value()) : extractNs(ns.dbName(), cmd);
}

BSONObj getObjWithSanitizedStorageEngineOptions(OperationContext* opCtx, const BSONObj& cmd) {
    static_assert(
        CreateCommand::kStorageEngineFieldName == IndexDescriptor::kStorageEngineFieldName,
        "Expected storage engine options field to be the same for collections and indexes.");

    if (auto storageEngineElem = cmd[IndexDescriptor::kStorageEngineFieldName]) {
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        auto engineObj = storageEngineElem.embeddedObject();
        auto sanitizedObj =
            storageEngine->getSanitizedStorageOptionsForSecondaryReplication(engineObj);
        return cmd.addFields(BSON(IndexDescriptor::kStorageEngineFieldName << sanitizedObj));
    }
    return cmd;
}

using OpApplyFn = std::function<Status(
    OperationContext* opCtx, const ApplierOperation& entry, OplogApplication::Mode mode)>;

struct ApplyOpMetadata {
    OpApplyFn applyFunc;
    // acceptableErrors are errors we accept for idempotency reasons.  Except for IndexNotFound,
    // they are only valid in non-steady-state oplog application modes.  IndexNotFound is always
    // allowed because index builds are not necessarily synchronized between secondary and primary.
    std::set<ErrorCodes::Error> acceptableErrors;

    ApplyOpMetadata(OpApplyFn fun) {
        applyFunc = fun;
    }

    ApplyOpMetadata(OpApplyFn fun, std::set<ErrorCodes::Error> theAcceptableErrors) {
        applyFunc = fun;
        acceptableErrors = theAcceptableErrors;
    }
};

const StringMap<ApplyOpMetadata> kOpsMap = {
    {"create",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto& ui = entry.getUuid();
          // Sanitize storage engine options to remove options which might not apply to this node.
          // See SERVER-68122.
          const auto cmd = getObjWithSanitizedStorageEngineOptions(opCtx, entry.getObject());
          const NamespaceString nss(extractNs(entry.getNss().dbName(), cmd));

          // Mode SECONDARY steady state replication should not allow create collection to rename an
          // existing collection out of the way. This leaves a collection orphaned and is a bug.
          // Renaming temporarily out of the way is only allowed for oplog replay, where we expect
          // any temporarily renamed aside collections to be sorted out by the time replay is
          // complete.
          const bool allowRenameOutOfTheWay = (mode != repl::OplogApplication::Mode::kSecondary);

          // Check whether there is an open but empty database where the name conflicts with the new
          // collection's database name. It is possible for a secondary's in-memory database state
          // to diverge from the primary's, if the primary rolls back the dropDatabase oplog entry
          // after closing its own in-memory database state. In this case, the primary may accept
          // creating a new database with a conflicting name to what the secondary still has open.
          // It is okay to simply close the empty database on the secondary in this case.
          if (auto duplicate =
                  DatabaseHolder::get(opCtx)->getNameWithConflictingCasing(nss.dbName())) {
              if (CollectionCatalog::get(opCtx)->getAllCollectionUUIDsFromDb(*duplicate).size() ==
                  0) {
                  fassert(7727801, dropDatabaseForApplyOps(opCtx, *duplicate).isOK());
              }
          }

          // If a change collection is to be created, that is, the change streams are being enabled
          // for a tenant, acquire exclusive tenant lock.
          Lock::DBLock dbLock(
              opCtx,
              nss.dbName(),
              MODE_IX,
              Date_t::max(),
              boost::make_optional(nss.tenantId() && nss.isChangeCollection(), MODE_X));

          if (auto idIndexElem = cmd["idIndex"]) {
              // Remove "idIndex" field from command.
              auto cmdWithoutIdIndex = cmd.removeField("idIndex");
              return createCollectionForApplyOps(opCtx,
                                                 nss.dbName(),
                                                 ui,
                                                 cmdWithoutIdIndex,
                                                 allowRenameOutOfTheWay,
                                                 idIndexElem.Obj());
          }

          // Collections clustered by _id do not need _id indexes.
          if (auto clusteredElem = cmd["clusteredIndex"]) {
              return createCollectionForApplyOps(
                  opCtx, nss.dbName(), ui, cmd, allowRenameOutOfTheWay, boost::none);
          }

          // No _id index spec was provided, so we should build a v:1 _id index.
          BSONObjBuilder idIndexSpecBuilder;
          idIndexSpecBuilder.append(IndexDescriptor::kIndexVersionFieldName,
                                    static_cast<int>(IndexVersion::kV1));
          idIndexSpecBuilder.append(IndexDescriptor::kIndexNameFieldName, "_id_");
          idIndexSpecBuilder.append(IndexDescriptor::kKeyPatternFieldName, BSON("_id" << 1));
          return createCollectionForApplyOps(
              opCtx, nss.dbName(), ui, cmd, allowRenameOutOfTheWay, idIndexSpecBuilder.done());
      },
      {ErrorCodes::NamespaceExists}}},
    {"createIndexes",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          // Sanitize storage engine options to remove options which might not apply to this node.
          // See SERVER-68122.
          const auto& entry = *op;
          const auto cmd = getObjWithSanitizedStorageEngineOptions(opCtx, entry.getObject());

          if (OplogApplication::Mode::kApplyOpsCmd == mode) {
              return {ErrorCodes::CommandNotSupported,
                      "The createIndexes operation is not supported in applyOps mode"};
          }

          const NamespaceString nss(
              extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd));
          BSONElement first = cmd.firstElement();
          invariant(first.fieldNameStringData() == "createIndexes");
          uassert(ErrorCodes::InvalidNamespace,
                  "createIndexes value must be a string",
                  first.type() == mongo::String);
          BSONObj indexSpec = cmd.removeField("createIndexes");
          Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
          boost::optional<Lock::CollectionLock> collLock;
          if (mongo::feature_flags::gCreateCollectionInPreparedTransactions.isEnabled(
                  serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
              opCtx->inMultiDocumentTransaction()) {
              // During initial sync we could have the following three scenarios:
              // * The collection is uncommitted and the index doesn't exist
              // * The collection already exists and the index doesn't exist
              // * Both exist
              //
              // The latter will cause us to return an IndexAlreadyExists error, which is an
              // acceptable error. The first one is the happy expected path so let's focus on the
              // other one. This case can only occur if the node is performing an initial sync and
              // the source node collection performed an index drop during a later part of the
              // oplog. In this scenario the index creation can early return since it knows the
              // index will be deleted at a later point.
              if (mode == OplogApplication::Mode::kInitialSync &&
                  !UncommittedCatalogUpdates::get(opCtx).isCreatedCollection(opCtx, nss)) {
                  return Status::OK();
              }

              // Multi-document transactions only allow createIndexes to implicitly create a
              // collection. In this case, the collection must be empty and uncommitted. We can
              // then relax the locking requirements (i.e. acquire the collection lock in MODE_IX)
              // to allow a prepared transaction with the uncommitted catalog write to stash its
              // resources before committing. This wouldn't be possible if we held the collection
              // lock in exclusive mode.
              invariant(UncommittedCatalogUpdates::get(opCtx).isCreatedCollection(opCtx, nss));
              collLock.emplace(opCtx, nss, MODE_IX);
          } else {
              collLock.emplace(opCtx, nss, MODE_X);
          }
          createIndexForApplyOps(opCtx, indexSpec, nss, mode);
          return Status::OK();
      },
      {ErrorCodes::IndexAlreadyExists,
       ErrorCodes::IndexBuildAlreadyInProgress,
       ErrorCodes::NamespaceNotFound}}},
    {"startIndexBuild",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          if (OplogApplication::Mode::kApplyOpsCmd == mode) {
              return {ErrorCodes::CommandNotSupported,
                      "The startIndexBuild operation is not supported in applyOps mode"};
          }

          const auto& entry = *op;
          auto swOplogEntry = IndexBuildOplogEntry::parse(entry);
          if (!swOplogEntry.isOK()) {
              return swOplogEntry.getStatus().withContext(
                  "Error parsing 'startIndexBuild' oplog entry");
          }

          // Sanitize storage engine options to remove options which might not apply to this node.
          // See SERVER-68122.
          for (auto& spec : swOplogEntry.getValue().indexSpecs) {
              spec = getObjWithSanitizedStorageEngineOptions(opCtx, spec);
          }

          IndexBuildsCoordinator::ApplicationMode applicationMode =
              IndexBuildsCoordinator::ApplicationMode::kNormal;
          if (mode == OplogApplication::Mode::kInitialSync) {
              applicationMode = IndexBuildsCoordinator::ApplicationMode::kInitialSync;
          }
          IndexBuildsCoordinator::get(opCtx)->applyStartIndexBuild(
              opCtx, applicationMode, swOplogEntry.getValue());
          return Status::OK();
      },
      {ErrorCodes::IndexAlreadyExists,
       ErrorCodes::IndexBuildAlreadyInProgress,
       ErrorCodes::NamespaceNotFound}}},
    {"commitIndexBuild",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          if (OplogApplication::Mode::kApplyOpsCmd == mode) {
              return {ErrorCodes::CommandNotSupported,
                      "The commitIndexBuild operation is not supported in applyOps mode"};
          }

          const auto& entry = *op;
          auto swOplogEntry = IndexBuildOplogEntry::parse(entry);
          if (!swOplogEntry.isOK()) {
              return swOplogEntry.getStatus().withContext(
                  "Error parsing 'commitIndexBuild' oplog entry");
          }
          auto* indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
          indexBuildsCoordinator->applyCommitIndexBuild(opCtx, swOplogEntry.getValue());
          return Status::OK();
      },
      {ErrorCodes::IndexAlreadyExists,
       ErrorCodes::IndexBuildAlreadyInProgress,
       ErrorCodes::NamespaceNotFound,
       ErrorCodes::NoSuchKey}}},
    {"abortIndexBuild",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          if (OplogApplication::Mode::kApplyOpsCmd == mode) {
              return {ErrorCodes::CommandNotSupported,
                      "The abortIndexBuild operation is not supported in applyOps mode"};
          }

          auto swOplogEntry = IndexBuildOplogEntry::parse(*op);
          if (!swOplogEntry.isOK()) {
              return swOplogEntry.getStatus().withContext(
                  "Error parsing 'abortIndexBuild' oplog entry");
          }
          IndexBuildsCoordinator::get(opCtx)->applyAbortIndexBuild(opCtx, swOplogEntry.getValue());
          return Status::OK();
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"collMod",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto& cmd = entry.getObject();

          const auto tenantId = entry.getNss().tenantId();
          const auto vts = tenantId
              ? auth::ValidatedTenancyScopeFactory::create(
                    *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{})
              : auth::ValidatedTenancyScope::kNotRequired;
          auto opMsg = OpMsgRequestBuilder::create(vts, entry.getNss().dbName(), cmd);
          auto collModCmd =
              CollMod::parse(IDLParserContext("collModOplogEntry",
                                              false /* apiStrict */,
                                              vts,
                                              tenantId,
                                              SerializationContext::stateStorageRequest()),
                             opMsg.body);
          const auto nssOrUUID([&collModCmd, &entry, mode]() -> NamespaceStringOrUUID {
              // Oplog entries from secondary oplog application will allways have the Uuid set and
              // it is only invocations of applyOps directly that may omit it
              if (!entry.getUuid()) {
                  invariant(mode == OplogApplication::Mode::kApplyOpsCmd);
                  return collModCmd.getNamespace();
              }

              return {collModCmd.getDbName(), *entry.getUuid()};
          }());
          return processCollModCommandForApplyOps(opCtx, nssOrUUID, collModCmd, mode);
      },
      {ErrorCodes::IndexNotFound, ErrorCodes::NamespaceNotFound}}},
    {"dbCheck",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status { return dbCheckOplogCommand(opCtx, *op, mode); },
      {}}},
    {"dropDatabase",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status { return dropDatabaseForApplyOps(opCtx, op->getNss().dbName()); },
      {ErrorCodes::NamespaceNotFound}}},
    {"drop",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto& cmd = entry.getObject();
          auto nss = extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd);
          if (nss.isDropPendingNamespace()) {
              LOGV2(21253,
                    "applyCommand: collection is already in a drop-pending state, ignoring "
                    "collection drop",
                    logAttrs(nss),
                    "command"_attr = redact(cmd));
              return Status::OK();
          }
          // Parse optime from oplog entry unless we are applying this command in standalone or on a
          // primary (replicated writes enabled).
          OpTime opTime;
          if (!opCtx->writesAreReplicated()) {
              opTime = entry.getOpTime();
          }
          return dropCollectionForApplyOps(
              opCtx, nss, opTime, DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
      },
      {ErrorCodes::NamespaceNotFound}}},
    // deleteIndex(es) is deprecated but still works as of April 10, 2015
    {"deleteIndex",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto& cmd = entry.getObject();
          return dropIndexesForApplyOps(
              opCtx, extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd), cmd);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"deleteIndexes",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto& cmd = entry.getObject();
          return dropIndexesForApplyOps(
              opCtx, extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd), cmd);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndex",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto& cmd = entry.getObject();
          return dropIndexesForApplyOps(
              opCtx, extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd), cmd);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndexes",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto& cmd = entry.getObject();
          return dropIndexesForApplyOps(
              opCtx, extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd), cmd);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"renameCollection",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          // Parse optime from oplog entry unless we are applying this command in standalone or on a
          // primary (replicated writes enabled).
          OpTime opTime;
          const auto& entry = *op;
          if (!opCtx->writesAreReplicated()) {
              opTime = entry.getOpTime();
          }
          return renameCollectionForApplyOps(
              opCtx, entry.getUuid(), entry.getTid(), entry.getObject(), opTime);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::NamespaceExists}}},
    {"importCollection",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto tenantId = entry.getNss().tenantId();
          const auto vts = tenantId
              ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
                    *tenantId,
                    auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
              : boost::none;
          auto importEntry = mongo::ImportCollectionOplogEntry::parse(
              IDLParserContext("importCollectionOplogEntry", false /* apiStrict */, vts, tenantId),
              entry.getObject());
          applyImportCollection(opCtx,
                                importEntry.getImportUUID(),
                                importEntry.getImportCollection(),
                                importEntry.getNumRecords(),
                                importEntry.getDataSize(),
                                importEntry.getCatalogEntry(),
                                importEntry.getStorageMetadata(),
                                importEntry.getDryRun(),
                                mode);
          return Status::OK();
      },
      {ErrorCodes::NamespaceExists}}},
    {"applyOps",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
         return op->shouldPrepare() ? applyPrepareTransaction(opCtx, op, mode)
                                    : applyApplyOpsOplogEntry(opCtx, *op, mode);
     }}},
    {"convertToCapped",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          const auto& cmd = entry.getObject();
          convertToCapped(opCtx,
                          extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd),
                          cmd["size"].safeNumberLong());
          return Status::OK();
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"emptycapped",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
          const auto& entry = *op;
          return emptyCapped(
              opCtx,
              extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), entry.getObject()));
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"commitTransaction",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
         return applyCommitTransaction(opCtx, op, mode);
     }}},
    {"abortTransaction",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
         return applyAbortTransaction(opCtx, op, mode);
     }}},
    {kShardingIndexCatalogOplogEntryName,
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
         const auto& entry = *op;
         auto indexCatalogOplog = ShardingIndexCatalogOplogEntry::parse(
             IDLParserContext("OplogModifyCollectionShardingIndexCatalogCtx"), entry.getObject());
         try {
             switch (indexCatalogOplog.getOp()) {
                 case ShardingIndexCatalogOpEnum::insert: {
                     auto indexEntry = ShardingIndexCatalogInsertEntry::parse(
                         IDLParserContext("OplogModifyCollectionShardingIndexCatalogCtx"),
                         entry.getObject());
                     addShardingIndexCatalogEntryToCollection(
                         opCtx,
                         entry.getNss(),
                         indexEntry.getI().getName().toString(),
                         indexEntry.getI().getKeyPattern(),
                         indexEntry.getI().getOptions(),
                         indexEntry.getI().getCollectionUUID(),
                         indexEntry.getI().getLastmod(),
                         indexEntry.getI().getIndexCollectionUUID());
                     break;
                 }
                 case ShardingIndexCatalogOpEnum::remove: {
                     auto removeEntry = ShardingIndexCatalogRemoveEntry::parse(
                         IDLParserContext("OplogModifyCatalogEntryContext"), entry.getObject());
                     removeShardingIndexCatalogEntryFromCollection(opCtx,
                                                                   entry.getNss(),
                                                                   removeEntry.getUuid(),
                                                                   removeEntry.getName(),
                                                                   removeEntry.getLastmod());
                     break;
                 }
                 case ShardingIndexCatalogOpEnum::replace: {
                     auto replaceEntry = ShardingIndexCatalogReplaceEntry::parse(
                         IDLParserContext("OplogModifyCatalogEntryContext"), entry.getObject());
                     replaceCollectionShardingIndexCatalog(opCtx,
                                                           entry.getNss(),
                                                           replaceEntry.getUuid(),
                                                           replaceEntry.getLastmod(),
                                                           replaceEntry.getI());
                     break;
                 }
                 case ShardingIndexCatalogOpEnum::clear: {
                     auto clearEntry = ShardingIndexCatalogClearEntry::parse(
                         IDLParserContext("OplogModifyCatalogEntryContext"), entry.getObject());
                     clearCollectionShardingIndexCatalog(
                         opCtx, entry.getNss(), clearEntry.getUuid());
                     break;
                 }
                 case ShardingIndexCatalogOpEnum::drop:
                     dropCollectionShardingIndexCatalog(opCtx, entry.getNss());
                     break;
                 case ShardingIndexCatalogOpEnum::rename: {
                     auto renameEntry = ShardingIndexCatalogRenameEntry::parse(
                         IDLParserContext("OplogModifyCatalogEntryContext"), entry.getObject());
                     renameCollectionShardingIndexCatalog(opCtx,
                                                          renameEntry.getFromNss(),
                                                          renameEntry.getToNss(),
                                                          renameEntry.getLastmod());
                     break;
                 }
                 default:
                     MONGO_UNREACHABLE;
             }
         } catch (const DBException& ex) {
             LOGV2_ERROR(6712302,
                         "Failed to apply modifyCollectionShardingIndexCatalog with entry obj",
                         "entry"_attr = redact(entry.getObject()),
                         "error"_attr = redact(ex));
             return ex.toStatus();
         }
         return Status::OK();
     }}},
    {"createGlobalIndex",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
         const auto& globalIndexUUID = op->getUuid().get();
         global_index::createContainer(opCtx, globalIndexUUID);
         return Status::OK();
     }}},
    {"dropGlobalIndex",
     {[](OperationContext* opCtx, const ApplierOperation& op, OplogApplication::Mode mode)
          -> Status {
         const auto& globalIndexUUID = op->getUuid().get();
         global_index::dropContainer(opCtx, globalIndexUUID);
         return Status::OK();
     }}},
};

// Writes a change stream pre-image 'preImage' associated with oplog entry 'oplogEntry' and a write
// operation to collection 'collection'. If we are writing the pre-image during oplog application
// on a secondary for a serverless tenant migration, we will use the timestamp and applyOpsIndex
// from the donor timeline. If we are applying this entry on a primary during tenant oplog
// application, we skip writing of the pre-image. The op observer will handle inserting the
// correct pre-image on the primary in this case.
void writeChangeStreamPreImage(OperationContext* opCtx,
                               const CollectionPtr& collection,
                               const mongo::repl::OplogEntry& oplogEntry,
                               const BSONObj& preImage) {
    Timestamp timestamp;
    int64_t applyOpsIndex;
    // If donorOpTime is set on the oplog entry, this is a write that is being applied on a
    // secondary during the oplog catchup phase of a tenant migration. Otherwise, we are either
    // applying a steady state write operation on a secondary or applying a write on the primary
    // during tenant migration oplog catchup.
    if (const auto& donorOpTime = oplogEntry.getDonorOpTime()) {
        timestamp = donorOpTime->getTimestamp();
        applyOpsIndex = oplogEntry.getDonorApplyOpsIndex().get_value_or(0);
    } else {
        timestamp = oplogEntry.getTimestampForPreImage();
        applyOpsIndex = oplogEntry.getApplyOpsIndex();
    }

    ChangeStreamPreImageId preImageId{collection->uuid(), timestamp, applyOpsIndex};
    ChangeStreamPreImage preImageDocument{
        std::move(preImageId), oplogEntry.getWallClockTimeForPreImage(), preImage};

    ChangeStreamPreImagesCollectionManager::get(opCtx).insertPreImage(
        opCtx, oplogEntry.getTid(), preImageDocument);
}
}  // namespace

constexpr StringData OplogApplication::kInitialSyncOplogApplicationMode;
constexpr StringData OplogApplication::kRecoveringOplogApplicationMode;
constexpr StringData OplogApplication::kStableRecoveringOplogApplicationMode;
constexpr StringData OplogApplication::kUnstableRecoveringOplogApplicationMode;
constexpr StringData OplogApplication::kSecondaryOplogApplicationMode;
constexpr StringData OplogApplication::kApplyOpsCmdOplogApplicationMode;

StringData OplogApplication::modeToString(OplogApplication::Mode mode) {
    switch (mode) {
        case OplogApplication::Mode::kInitialSync:
            return OplogApplication::kInitialSyncOplogApplicationMode;
        case OplogApplication::Mode::kUnstableRecovering:
            return OplogApplication::kUnstableRecoveringOplogApplicationMode;
        case OplogApplication::Mode::kStableRecovering:
            return OplogApplication::kStableRecoveringOplogApplicationMode;
        case OplogApplication::Mode::kSecondary:
            return OplogApplication::kSecondaryOplogApplicationMode;
        case OplogApplication::Mode::kApplyOpsCmd:
            return OplogApplication::kApplyOpsCmdOplogApplicationMode;
    }
    MONGO_UNREACHABLE;
}

StatusWith<OplogApplication::Mode> OplogApplication::parseMode(const std::string& mode) {
    if (mode == OplogApplication::kInitialSyncOplogApplicationMode) {
        return OplogApplication::Mode::kInitialSync;
    } else if (mode == OplogApplication::kRecoveringOplogApplicationMode) {
        // This only being used in applyOps command which is controlled by the client, so it should
        // be unstable.
        return OplogApplication::Mode::kUnstableRecovering;
    } else if (mode == OplogApplication::kSecondaryOplogApplicationMode) {
        return OplogApplication::Mode::kSecondary;
    } else if (mode == OplogApplication::kApplyOpsCmdOplogApplicationMode) {
        return OplogApplication::Mode::kApplyOpsCmd;
    } else {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Invalid oplog application mode provided: " << mode);
    }
    MONGO_UNREACHABLE;
}

void OplogApplication::checkOnOplogFailureForRecovery(OperationContext* opCtx,
                                                      const mongo::NamespaceString& nss,
                                                      const mongo::BSONObj& oplogEntry,
                                                      const std::string& errorMsg) {
    const bool isReplicaSet =
        repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getSettings().isReplSet();
    // Relax the constraints of oplog application if the node is not a replica set member or the
    // node is in the middle of a backup and restore process.
    if (!isReplicaSet || storageGlobalParams.restore) {
        return;
    }

    // During the recovery process, certain configuration collections such as
    // 'config.image_collections' are handled differently, which may result in encountering oplog
    // application failures in common scenarios, and therefore assert statements are not used.
    if (nss.isConfigDB()) {
        LOGV2_DEBUG(
            5415002,
            1,
            "Error applying operation while recovering from stable checkpoint. This is related to "
            "one of the configuration collections so this error might be benign.",
            "oplogEntry"_attr = oplogEntry,
            "error"_attr = errorMsg);
    } else if (getTestCommandsEnabled()) {
        // Only fassert in test environment.
        LOGV2_FATAL(5415000,
                    "Error applying operation while recovering from stable "
                    "checkpoint. This can lead to data corruption.",
                    "oplogEntry"_attr = oplogEntry,
                    "error"_attr = errorMsg);
    } else {
        LOGV2_WARNING(5415001,
                      "Error applying operation while recovering from stable "
                      "checkpoint. This can lead to data corruption.",
                      "oplogEntry"_attr = oplogEntry,
                      "error"_attr = errorMsg);
    }
}

// Logger for oplog constraint violations.
OplogConstraintViolationLogger* oplogConstraintViolationLogger;

MONGO_INITIALIZER(CreateOplogConstraintViolationLogger)(InitializerContext* context) {
    oplogConstraintViolationLogger = new OplogConstraintViolationLogger();
}

void logOplogConstraintViolation(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OplogConstraintViolationEnum type,
                                 const std::string& operation,
                                 const BSONObj& opObj,
                                 boost::optional<Status> status) {
    // Log the violation.
    oplogConstraintViolationLogger->logViolationIfReady(type, opObj, status);

    // Write a new entry to the health log.
    HealthLogEntry entry;
    entry.setNss(nss);
    entry.setTimestamp(Date_t::now());
    // Oplog constraint violations should always be marked as warning.
    entry.setSeverity(SeverityEnum::Warning);
    entry.setScope(ScopeEnum::Document);
    entry.setMsg(toString(type));
    entry.setOperation(operation);
    entry.setData(opObj);

    HealthLogInterface::get(opCtx->getServiceContext())->log(entry);
}

// @return failure status if an update should have happened and the document DNE.
// See replset initial sync code.
Status applyOperation_inlock(OperationContext* opCtx,
                             CollectionAcquisition& collectionAcquisition,
                             const OplogEntryOrGroupedInserts& opOrGroupedInserts,
                             bool alwaysUpsert,
                             OplogApplication::Mode mode,
                             const bool isDataConsistent,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats) {
    // Get the single oplog entry to be applied or the first oplog entry of grouped inserts.
    const auto& op = *opOrGroupedInserts.getOp();
    LOGV2_DEBUG(21254,
                3,
                "Applying op (or grouped inserts)",
                "op"_attr = redact(opOrGroupedInserts.toBSON()),
                "oplogApplicationMode"_attr = OplogApplication::modeToString(mode));

    // Choose opCounters based on running on standalone/primary or secondary by checking
    // whether writes are replicated.
    const bool shouldUseGlobalOpCounters =
        mode == repl::OplogApplication::Mode::kApplyOpsCmd || opCtx->writesAreReplicated();
    OpCounters* opCounters = shouldUseGlobalOpCounters ? &globalOpCounters : &replOpCounters;

    auto opType = op.getOpType();
    if (opType == OpTypeEnum::kNoop) {
        // no op
        if (incrementOpsAppliedStats) {
            incrementOpsAppliedStats();
        }

        return Status::OK();
    }

    const bool inStableRecovery = mode == OplogApplication::Mode::kStableRecovering;
    NamespaceString requestNss;
    if (auto uuid = op.getUuid()) {
        auto catalog = CollectionCatalog::get(opCtx);
        const auto collection = CollectionPtr(catalog->lookupCollectionByUUID(opCtx, uuid.value()));
        if (!collection && inStableRecovery) {
            repl::OplogApplication::checkOnOplogFailureForRecovery(
                opCtx,
                op.getNss(),
                redact(op.toBSONForLogging()),
                str::stream()
                    << "(NamespaceNotFound): Failed to apply operation due to missing collection ("
                    << uuid.value() << ")");
        }

        // Invalidate the image collection if collectionUUID does not resolve and this op returns
        // a preimage or postimage. We only expect this to happen when in kInitialSync mode but
        // this can sometimes occur in recovering mode during rollback-via-refetch. In either case
        // we want to do image invalidation.
        if (!collection && op.getNeedsRetryImage()) {
            tassert(735200,
                    "mode should be in initialSync or recovering",
                    mode == OplogApplication::Mode::kInitialSync ||
                        OplogApplication::inRecovering(mode));
            writeConflictRetryWithLimit(opCtx, "applyOps_imageInvalidation", op.getNss(), [&] {
                WriteUnitOfWork wuow(opCtx);
                writeToImageCollection(opCtx,
                                       op.getSessionId().value(),
                                       op.getTxnNumber().value(),
                                       op.getApplyOpsTimestamp().value_or(op.getTimestamp()),
                                       op.getNeedsRetryImage().value(),
                                       BSONObj(),
                                       getInvalidatingReason(mode, isDataConsistent));
                wuow.commit();
            });
        }
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply operation due to missing collection ("
                              << uuid.value() << "): " << redact(opOrGroupedInserts.toBSON()),
                collection);
        requestNss = collection->ns();
        dassert(requestNss == collectionAcquisition.nss());
        dassert(
            shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(requestNss, MODE_IX));
    } else {
        requestNss = op.getNss();
        invariant(requestNss.coll().size());
        dassert(
            shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(requestNss, MODE_IX),
            requestNss.toStringForErrorMsg());
    }

    const CollectionPtr& collection = collectionAcquisition.getCollectionPtr();

    assertInitialSyncCanContinueDuringShardMerge(opCtx, requestNss, op);

    BSONObj o = op.getObject();

    // The feature compatibility version in the server configuration collection must not change
    // during initial sync.
    if ((mode == OplogApplication::Mode::kInitialSync) &&
        requestNss == NamespaceString::kServerConfigurationNamespace) {
        std::string oID;
        auto status = bsonExtractStringField(o, "_id", &oID);
        if (status.isOK() && oID == multiversion::kParameterName) {
            return Status(ErrorCodes::OplogOperationUnsupported,
                          str::stream() << "Applying operation on feature compatibility version "
                                           "document not supported in initial sync: "
                                        << redact(opOrGroupedInserts.toBSON()));
        }
    }

    BSONObj o2;
    if (op.getObject2())
        o2 = op.getObject2().value();

    const IndexCatalog* indexCatalog = !collection ? nullptr : collection->getIndexCatalog();
    const bool haveWrappingWriteUnitOfWork =
        shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork();
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "applyOps not supported on view: " << requestNss.toStringForErrorMsg(),
            collection || !CollectionCatalog::get(opCtx)->lookupView(opCtx, requestNss));

    // Decide whether to timestamp the write with the 'ts' field found in the operation. In general,
    // we do this for secondary oplog application, but there are some exceptions.
    const bool assignOperationTimestamp = [opCtx, haveWrappingWriteUnitOfWork, mode] {
        if (opCtx->writesAreReplicated()) {
            // We do not assign timestamps on replicated writes since they will get their oplog
            // timestamp once they are logged. The operation may contain a timestamp if it is part
            // of a applyOps command, but we ignore it so that we don't violate oplog ordering.
            return false;
        } else if (haveWrappingWriteUnitOfWork) {
            // We do not assign timestamps to non-replicated writes that have a wrapping
            // WriteUnitOfWork, as they will get the timestamp on that WUOW. Use cases include:
            // Secondary oplog application of prepared transactions.
            return false;
        } else if (ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
            // Secondary oplog application not in a WUOW uses the timestamp in the operation
            // document.
            return true;
        } else {
            // Only assign timestamps on standalones during replication recovery when
            // started with the 'recoverFromOplogAsStandalone' flag.
            return OplogApplication::inRecovering(mode);
        }
        MONGO_UNREACHABLE;
    }();
    invariant(!assignOperationTimestamp || !op.getTimestamp().isNull(),
              str::stream() << "Oplog entry did not have 'ts' field when expected: "
                            << redact(opOrGroupedInserts.toBSON()));

    auto shouldRecordChangeStreamPreImage = [&]() {
        // Should record a change stream pre-image when:
        // (1) the state of the collection is guaranteed to be consistent so it is possible to
        // compute a correct pre-image,
        // (2) and the oplog entry is not a result of chunk migration or collection resharding -
        // such entries do not get reflected as change events and it is not possible to compute a
        // correct pre-image for them.
        return collection && collection->isChangeStreamPreAndPostImagesEnabled() &&
            isDataConsistent &&
            (OplogApplication::inRecovering(mode) || mode == OplogApplication::Mode::kSecondary) &&
            !op.getFromMigrate().get_value_or(false) &&
            !requestNss.isTemporaryReshardingCollection();
    };


    // We are applying this entry on the primary during tenant oplog application. Decorate the opCtx
    // with donor timeline metadata so that it will be available in the op observer and available
    // for use here when oplog entries are logged.
    if (auto& recipientInfo = tenantMigrationInfo(opCtx)) {
        recipientInfo->donorOplogEntryData =
            DonorOplogEntryData(op.getOpTime(), op.getApplyOpsIndex());
    }

    switch (opType) {
        case OpTypeEnum::kInsert: {
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Failed to apply insert due to missing collection: "
                                  << redact(opOrGroupedInserts.toBSON()),
                    collection);
            if (opOrGroupedInserts.isGroupedInserts()) {
                // Grouped inserts.

                // Cannot apply an array insert with applyOps command.  But can apply grouped
                // inserts on primary as part of a tenant migration.
                uassert(ErrorCodes::OperationFailed,
                        "Cannot apply an array insert with applyOps",
                        !opCtx->writesAreReplicated() || tenantMigrationInfo(opCtx));

                std::vector<InsertStatement> insertObjs;
                const auto insertOps = opOrGroupedInserts.getGroupedInserts();
                WriteUnitOfWork wuow(opCtx);
                if (!opCtx->writesAreReplicated()) {
                    for (const auto& iOp : insertOps) {
                        invariant(iOp->getTerm());
                        insertObjs.emplace_back(
                            iOp->getObject(), iOp->getTimestamp(), iOp->getTerm().value());
                    }
                } else {
                    // Applying grouped inserts on the primary as part of a tenant migration.
                    // We assign new optimes as the optimes on the donor are not relevant to
                    // the recipient.
                    std::vector<OplogSlot> slots = getNextOpTimes(opCtx, insertOps.size());
                    auto slotIter = slots.begin();
                    for (const auto& iOp : insertOps) {
                        insertObjs.emplace_back(
                            iOp->getObject(), slotIter->getTimestamp(), slotIter->getTerm());
                        slotIter++;
                    }
                }

                // If an oplog entry has a recordId, this means that the collection is a
                // recordIdReplicated collection, and therefore we should use the recordId
                // present.
                // Because this code is run on secondaries as well as on primaries that use
                // applyOps, this has the effect of preserving recordIds when applyOps is run,
                // which is intentional.
                for (size_t i = 0; i < insertObjs.size(); i++) {
                    if (insertOps[i]->getDurableReplOperation().getRecordId()) {
                        insertObjs[i].replicatedRecordId =
                            *insertOps[i]->getDurableReplOperation().getRecordId();
                    }
                }

                OpDebug* const nullOpDebug = nullptr;
                Status status = collection_internal::insertDocuments(opCtx,
                                                                     collection,
                                                                     insertObjs.begin(),
                                                                     insertObjs.end(),
                                                                     nullOpDebug,
                                                                     false /* fromMigrate */);
                if (!status.isOK()) {
                    return status;
                }
                wuow.commit();
                for (size_t i = 0; i < insertObjs.size(); i++) {
                    opCounters->gotInsert();
                    if (shouldUseGlobalOpCounters) {
                        ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
                            opCtx->getWriteConcern());
                    }
                    if (incrementOpsAppliedStats) {
                        incrementOpsAppliedStats();
                    }
                }
            } else {
                // Single insert.
                opCounters->gotInsert();
                if (shouldUseGlobalOpCounters) {
                    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
                        opCtx->getWriteConcern());
                }

                // No _id.
                // This indicates an issue with the upstream server:
                //     The oplog entry is corrupted; or
                //     The version of the upstream server is obsolete.
                uassert(ErrorCodes::NoSuchKey,
                        str::stream() << "Failed to apply insert due to missing _id: "
                                      << redact(op.toBSONForLogging()),
                        o.hasField("_id"));

                // 1. Insert if
                //   a) we do not have a wrapping WriteUnitOfWork, which implies we are not part of
                //      an "applyOps" command, OR
                //   b) we are part of a multi-document transaction[1], OR
                //
                // 2. Upsert[2] if
                //   a) we have a wrapping WriteUnitOfWork AND we are not part of a transaction,
                //      which implies we are part of an "applyOps" command, OR
                //   b) the previous insert failed with a DuplicateKey error AND we are not part
                //      a transaction AND either we are not in steady state replication mode OR
                //      the oplogApplicationEnforcesSteadyStateConstraints parameter is false.
                //
                // [1] Transactions should not convert inserts to upserts because on secondaries
                //     they will perform a lookup that never occurred on the primary. This may cause
                //     an unintended prepare conflict and block replication. For this reason,
                //     transactions should always fail with DuplicateKey errors and never retry
                //     inserts as upserts.
                // [2] This upsert behavior exists to support idempotency guarantees outside
                //     steady-state replication and existing users of applyOps.

                const bool inTxn = opCtx->inMultiDocumentTransaction();
                bool needToDoUpsert = haveWrappingWriteUnitOfWork && !inTxn;

                Timestamp timestamp;
                if (assignOperationTimestamp) {
                    timestamp = op.getTimestamp();
                }

                if (!needToDoUpsert) {
                    WriteUnitOfWork wuow(opCtx);

                    // Do not use supplied timestamps if running through applyOps, as that would
                    // allow a user to dictate what timestamps appear in the oplog.
                    InsertStatement insertStmt(o);
                    if (assignOperationTimestamp) {
                        invariant(op.getTerm());
                        insertStmt.oplogSlot = OpTime(op.getTimestamp(), op.getTerm().value());
                    } else if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(
                                   opCtx, collection->ns())) {
                        // Primaries processing inserts always pre-allocate timestamps. For parity,
                        // we also pre-allocate timestamps for an `applyOps` of insert oplog
                        // entries. This parity is meaningful for capped collections where the
                        // insert may result in a delete that becomes replicated.
                        auto oplogInfo = LocalOplogInfo::get(opCtx);
                        auto oplogSlots = oplogInfo->getNextOpTimes(opCtx, /*batchSize=*/1);
                        insertStmt.oplogSlot = oplogSlots.front();
                    }

                    // If an oplog entry has a recordId, this means that the collection is a
                    // recordIdReplicated collection, and therefore we should use the recordId
                    // present.
                    if (op.getDurableReplOperation().getRecordId()) {
                        insertStmt.replicatedRecordId = *op.getDurableReplOperation().getRecordId();
                    }

                    OpDebug* const nullOpDebug = nullptr;
                    Status status = collection_internal::insertDocument(
                        opCtx, collection, insertStmt, nullOpDebug, false /* fromMigrate */);

                    if (status.isOK()) {
                        wuow.commit();
                    } else if (status == ErrorCodes::DuplicateKey) {
                        // Transactions cannot be retried as upserts once they fail with a duplicate
                        // key error.
                        if (inTxn) {
                            return status;
                        }
                        if (mode == OplogApplication::Mode::kSecondary) {
                            const auto& opObj = redact(op.toBSONForLogging());

                            opCounters->gotInsertOnExistingDoc();
                            logOplogConstraintViolation(
                                opCtx,
                                op.getNss(),
                                OplogConstraintViolationEnum::kInsertOnExistingDoc,
                                "insert",
                                opObj,
                                boost::none /* status */);

                            if (oplogApplicationEnforcesSteadyStateConstraints) {
                                return status;
                            }
                        } else if (inStableRecovery) {
                            repl::OplogApplication::checkOnOplogFailureForRecovery(
                                opCtx, op.getNss(), redact(op.toBSONForLogging()), redact(status));
                        }
                        // Continue to the next block to retry the operation as an upsert.
                        needToDoUpsert = true;
                    } else {
                        return status;
                    }
                }

                // Now see if we need to do an upsert.
                if (needToDoUpsert) {
                    // Do update on DuplicateKey errors.
                    // This will only be on the _id field in replication,
                    // since we disable non-_id unique constraint violations.
                    BSONObjBuilder b;
                    b.append(o.getField("_id"));

                    auto request = UpdateRequest();
                    request.setNamespaceString(requestNss);
                    request.setQuery(b.done());
                    request.setUpdateModification(
                        write_ops::UpdateModification::parseFromClassicUpdate(o));
                    request.setUpsert();
                    request.setFromOplogApplication(true);

                    writeConflictRetryWithLimit(opCtx, "applyOps_upsert", op.getNss(), [&] {
                        WriteUnitOfWork wuow(opCtx);
                        // If `haveWrappingWriteUnitOfWork` is true, do not timestamp the write.
                        if (assignOperationTimestamp && timestamp != Timestamp::min()) {
                            uassertStatusOK(
                                shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(
                                    timestamp));
                        }

                        UpdateResult res = update(opCtx, collectionAcquisition, request);
                        if (res.numMatched == 0 && res.upsertedId.isEmpty()) {
                            LOGV2_ERROR(21257,
                                        "No document was updated even though we got a DuplicateKey "
                                        "error when inserting");
                            fassertFailedNoTrace(28750);
                        }
                        wuow.commit();
                    });
                }

                if (incrementOpsAppliedStats) {
                    incrementOpsAppliedStats();
                }
            }
            break;
        }
        case OpTypeEnum::kUpdate: {
            opCounters->gotUpdate();
            if (shouldUseGlobalOpCounters) {
                ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForUpdate(
                    opCtx->getWriteConcern());
            }

            auto idField = o2["_id"];
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "Failed to apply update due to missing _id: "
                                  << redact(op.toBSONForLogging()),
                    !idField.eoo());

            // The o2 field may contain additional fields besides the _id (like the shard key
            // fields), but we want to do the update by just _id so we can take advantage of the
            // IDHACK.
            BSONObj updateCriteria = idField.wrap();

            const bool upsertOplogEntry = op.getUpsert().value_or(false);
            const bool upsert = alwaysUpsert || upsertOplogEntry;
            auto request = UpdateRequest();
            request.setNamespaceString(requestNss);
            request.setQuery(updateCriteria);
            // If we are in steady state and the update is on a timeseries bucket collection, we can
            // enable some optimizations in diff application. In some cases, like during tenant
            // migration or $_internalApplyOplogUpdate update, we can for some reason generate
            // entries for timeseries bucket collections which still rely on the idempotency
            // guarantee, which then means we shouldn't apply these optimizations.
            write_ops::UpdateModification::DiffOptions options;
            if (mode == OplogApplication::Mode::kSecondary && collection->getTimeseriesOptions() &&
                !op.getCheckExistenceForDiffInsert() && !op.getFromTenantMigration()) {
                options.mustCheckExistenceForInsertOperations = false;
            }
            auto updateMod = write_ops::UpdateModification::parseFromOplogEntry(o, options);

            request.setUpdateModification(std::move(updateMod));
            request.setUpsert(upsert);
            request.setFromOplogApplication(true);
            if (mode != OplogApplication::Mode::kInitialSync && isDataConsistent) {
                if (op.getNeedsRetryImage() == repl::RetryImageEnum::kPreImage) {
                    request.setReturnDocs(UpdateRequest::ReturnDocOption::RETURN_OLD);
                } else if (op.getNeedsRetryImage() == repl::RetryImageEnum::kPostImage) {
                    request.setReturnDocs(UpdateRequest::ReturnDocOption::RETURN_NEW);
                }
            }

            // Determine if a change stream pre-image has to be recorded for the oplog entry.
            const bool recordChangeStreamPreImage = shouldRecordChangeStreamPreImage();
            BSONObj changeStreamPreImage;
            if (recordChangeStreamPreImage && !request.shouldReturnNewDocs()) {
                // The new version of the document to be loaded was not requested - request
                // returning of the document version before update to be used as change stream
                // pre-image.
                request.setReturnDocs(UpdateRequest::ReturnDocOption::RETURN_OLD);
            }

            Timestamp timestamp;
            if (assignOperationTimestamp) {
                timestamp = op.getTimestamp();
            }

            // Operations that were part of a retryable findAndModify have two formats for
            // replicating pre/post images. The classic format has primaries writing explicit noop
            // oplog entries that contain the necessary details for reconstructed a response to a
            // retried operation.
            //
            // In the new format, we "implicitly" replicate the necessary data. Oplog entries may
            // contain an optional field, `needsRetryImage` with a value of `preImage` or
            // `postImage`. When applying these oplog entries, we also retrieve the pre/post image
            // retrieved by the query system and write that value into `config.image_collection` as
            // part of the same oplog application transaction. The `config.image_collection`
            // documents are keyed by the oplog entries logical session id, which is the same as the
            // `config.transactions` table.
            //
            // Batches of oplog entries can contain multiple oplog entries from the same logical
            // session. Thus updates to `config.image_collection` documents can be
            // concurrent. Secondaries already coalesce (read: intentionally ignore) some writes to
            // `config.transactions`, we may also omit some writes to `config.image_collection`, so
            // long as the last write persists.  We handle this in the oplog applier by allowing
            // only one write (whether implicit update or explicit delete) to
            // config.image_collection per applier batch.  This works in the case of rollback
            // because we are only allowed to retry the latest find-and-modify operation on a
            // session; any images lost to rollback would have had the wrong transaction ID and
            // thus not been legal to use anyway
            //
            // On the primary, we are assured the writes to this collection are ordered because
            // the logical session ID being written is checked out when we do the write.
            // We can still get a write conflict on the primary as a delete done as part of expired
            // session cleanup can race with a use of the expired session.
            auto status = writeConflictRetryWithLimit(opCtx, "applyOps_update", op.getNss(), [&] {
                WriteUnitOfWork wuow(opCtx);
                if (timestamp != Timestamp::min()) {
                    uassertStatusOK(
                        shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(timestamp));
                }

                if (recordChangeStreamPreImage && request.shouldReturnNewDocs()) {
                    // Load the document version before update to be used as the change stream
                    // pre-image since the update operation will load the new version of the
                    // document.
                    invariant(op.getObject2());
                    auto&& documentId = *op.getObject2();

                    auto documentFound = Helpers::findById(
                        opCtx, collection->ns(), documentId, changeStreamPreImage);
                    invariant(documentFound);
                }

                UpdateResult ur = update(opCtx, collectionAcquisition, request);
                if (ur.numMatched == 0 && ur.upsertedId.isEmpty()) {
                    if (collection && collection->isCapped() &&
                        mode == OplogApplication::Mode::kSecondary) {
                        // We can't assume there was a problem when the collection is capped,
                        // because the item may have been deleted by the cappedDeleter.  This
                        // only matters for steady-state mode, because all errors on missing
                        // updates are ignored at a higher level for recovery and initial sync.
                        LOGV2_DEBUG(2170003,
                                    2,
                                    "couldn't find doc in capped collection",
                                    "op"_attr = redact(op.toBSONForLogging()));
                    } else if (ur.modifiers) {
                        if (updateCriteria.nFields() == 1) {
                            // was a simple { _id : ... } update criteria
                            static constexpr char msg[] = "Failed to apply update";
                            LOGV2_ERROR(21258, msg, "op"_attr = redact(op.toBSONForLogging()));
                            return Status(ErrorCodes::UpdateOperationFailed,
                                          str::stream()
                                              << msg << ": " << redact(op.toBSONForLogging()));
                        }

                        // Need to check to see if it isn't present so we can exit early with a
                        // failure. Note that adds some overhead for this extra check in some
                        // cases, such as an updateCriteria of the form { _id:..., { x :
                        // {$size:...} } thus this is not ideal.
                        if (!collection ||
                            (indexCatalog->haveIdIndex(opCtx) &&
                             Helpers::findById(opCtx, collection, updateCriteria).isNull()) ||
                            // capped collections won't have an _id index
                            (!indexCatalog->haveIdIndex(opCtx) &&
                             Helpers::findOne(opCtx, collection, updateCriteria).isNull())) {
                            static constexpr char msg[] = "Couldn't find document";
                            LOGV2_ERROR(21259, msg, "op"_attr = redact(op.toBSONForLogging()));
                            return Status(ErrorCodes::UpdateOperationFailed,
                                          str::stream()
                                              << msg << ": " << redact(op.toBSONForLogging()));
                        }

                        // Otherwise, it's present; zero objects were updated because of
                        // additional specifiers in the query for idempotence
                    } else {
                        // this could happen benignly on an oplog duplicate replay of an upsert
                        // (because we are idempotent), if a regular non-mod update fails the
                        // item is (presumably) missing.
                        if (!upsert) {
                            static constexpr char msg[] = "Update of non-mod failed";
                            LOGV2_ERROR(21260, msg, "op"_attr = redact(op.toBSONForLogging()));
                            return Status(ErrorCodes::UpdateOperationFailed,
                                          str::stream()
                                              << msg << ": " << redact(op.toBSONForLogging()));
                        }
                    }
                } else if (mode == OplogApplication::Mode::kSecondary && !upsertOplogEntry &&
                           !ur.upsertedId.isEmpty() && !(collection && collection->isCapped())) {
                    // This indicates we upconverted an update to an upsert, and it did indeed
                    // upsert.  In steady state mode this is unexpected.
                    const auto& opObj = redact(op.toBSONForLogging());

                    opCounters->gotUpdateOnMissingDoc();
                    logOplogConstraintViolation(opCtx,
                                                op.getNss(),
                                                OplogConstraintViolationEnum::kUpdateOnMissingDoc,
                                                "update",
                                                opObj,
                                                boost::none /* status */);

                    // We shouldn't be doing upserts in secondary mode when enforcing steady
                    // state constraints.
                    invariant(!oplogApplicationEnforcesSteadyStateConstraints);
                }

                if (op.getNeedsRetryImage()) {
                    writeToImageCollection(opCtx,
                                           op.getSessionId().value(),
                                           op.getTxnNumber().value(),
                                           op.getApplyOpsTimestamp().value_or(op.getTimestamp()),
                                           op.getNeedsRetryImage().value(),
                                           // If we did not request an image because we're in
                                           // initial sync, the value passed in here is conveniently
                                           // the empty BSONObj.
                                           ur.requestedDocImage,
                                           getInvalidatingReason(mode, isDataConsistent));
                }

                if (recordChangeStreamPreImage) {
                    if (!request.shouldReturnNewDocs()) {
                        // A document version before update was loaded by the update operation.
                        invariant(!ur.requestedDocImage.isEmpty());
                        changeStreamPreImage = ur.requestedDocImage;
                    }

                    // Write a pre-image of a document for change streams.
                    writeChangeStreamPreImage(opCtx, collection, op, changeStreamPreImage);
                }

                wuow.commit();
                return Status::OK();
            });

            if (!status.isOK()) {
                if (inStableRecovery) {
                    repl::OplogApplication::checkOnOplogFailureForRecovery(
                        opCtx, op.getNss(), redact(op.toBSONForLogging()), redact(status));
                }
                return status;
            }

            if (incrementOpsAppliedStats) {
                incrementOpsAppliedStats();
            }
            break;
        }
        case OpTypeEnum::kDelete: {
            opCounters->gotDelete();
            if (shouldUseGlobalOpCounters) {
                ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForDelete(
                    opCtx->getWriteConcern());
            }

            auto idField = o["_id"];
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "Failed to apply delete due to missing _id: "
                                  << redact(op.toBSONForLogging()),
                    !idField.eoo());

            // The o field may contain additional fields besides the _id (like the shard key
            // fields), but we want to do the delete by just _id so we can take advantage of the
            // IDHACK.
            BSONObj deleteCriteria = idField.wrap();

            Timestamp timestamp;
            if (assignOperationTimestamp) {
                timestamp = op.getTimestamp();
            }

            // Determine if a change stream pre-image has to be recorded for the oplog entry.
            const bool recordChangeStreamPreImage = shouldRecordChangeStreamPreImage();

            writeConflictRetryWithLimit(opCtx, "applyOps_delete", op.getNss(), [&] {
                WriteUnitOfWork wuow(opCtx);
                if (timestamp != Timestamp::min()) {
                    uassertStatusOK(
                        shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(timestamp));
                }

                DeleteRequest request;
                request.setNsString(requestNss);
                request.setQuery(deleteCriteria);
                if (mode != OplogApplication::Mode::kInitialSync &&
                    op.getNeedsRetryImage() == repl::RetryImageEnum::kPreImage &&
                    isDataConsistent) {
                    // When in initial sync, we'll pass an empty image into
                    // `writeToImageCollection`.
                    request.setReturnDeleted(true);
                }

                if (recordChangeStreamPreImage) {
                    // Request loading of the document version before delete operation to be
                    // used as change stream pre-image.
                    request.setReturnDeleted(true);
                }

                DeleteResult result = deleteObject(opCtx, collectionAcquisition, request);
                if (op.getNeedsRetryImage()) {
                    // Even if `result.nDeleted` is 0, we want to perform a write to the
                    // imageCollection to advance the txnNumber/ts and invalidate the image.
                    // This isn't strictly necessary for correctness -- the
                    // `config.transactions` table is responsible for whether to retry. The
                    // motivation here is to simply reduce the number of states related
                    // documents in the two collections can be in.
                    writeToImageCollection(opCtx,
                                           op.getSessionId().value(),
                                           op.getTxnNumber().value(),
                                           op.getApplyOpsTimestamp().value_or(op.getTimestamp()),
                                           repl::RetryImageEnum::kPreImage,
                                           result.requestedPreImage.value_or(BSONObj()),
                                           getInvalidatingReason(mode, isDataConsistent));
                }

                if (recordChangeStreamPreImage) {
                    invariant(result.requestedPreImage);
                    writeChangeStreamPreImage(opCtx, collection, op, *(result.requestedPreImage));
                }

                if (result.nDeleted == 0 && inStableRecovery) {
                    repl::OplogApplication::checkOnOplogFailureForRecovery(
                        opCtx,
                        op.getNss(),
                        redact(op.toBSONForLogging()),
                        !collection ? str::stream()
                                << "(NamespaceNotFound): Failed to apply operation due "
                                   "to missing collection ("
                                << requestNss.toStringForErrorMsg() << ")"
                                    : "Applied a delete which did not delete anything."s);
                }
                // It is legal for a delete operation on the pre-images collection to delete
                // zero documents - pre-image collections are not guaranteed to contain the same
                // set of documents at all times. The same holds for change-collections as they
                // both rely on unreplicated deletes when
                // "featureFlagUseUnreplicatedTruncatesForDeletions" is enabled.
                //
                // TODO SERVER-70591: Remove feature flag requirement in comment above.
                //
                // It is also legal for a delete operation on the config.image_collection (used
                // for find-and-modify retries) to delete zero documents.  Since we do not write
                // updates to this collection which are in the same batch as later deletes, a
                // rollback to the middle of a batch with both an update and a delete may result
                // in a missing document, which may be later deleted.
                if (result.nDeleted == 0 && mode == OplogApplication::Mode::kSecondary &&
                    !requestNss.isChangeStreamPreImagesCollection() &&
                    !requestNss.isChangeCollection() && !requestNss.isConfigImagesCollection()) {
                    // In FCV 4.4, each node is responsible for deleting the excess documents in
                    // capped collections. This implies that capped deletes may not be
                    // synchronized between nodes at times. When upgraded to FCV 5.0, the
                    // primary will generate delete oplog entries for capped collections.
                    // However, if any secondary was behind in deleting excess documents while
                    // in FCV 4.4, the primary would have no way of knowing and it would delete
                    // the first document it sees locally. Eventually, when secondaries step up
                    // and start deleting capped documents, they will first delete previously
                    // missed documents that may already be deleted on other nodes. For this
                    // reason we skip returning NoSuchKey for capped collections when oplog
                    // application is enforcing steady state constraints.
                    bool isCapped = false;
                    const auto& opObj = redact(op.toBSONForLogging());
                    if (collection) {
                        isCapped = collection->isCapped();
                        opCounters->gotDeleteWasEmpty();
                        logOplogConstraintViolation(opCtx,
                                                    op.getNss(),
                                                    OplogConstraintViolationEnum::kDeleteWasEmpty,
                                                    "delete",
                                                    opObj,
                                                    boost::none /* status */);
                    } else {
                        opCounters->gotDeleteFromMissingNamespace();
                        logOplogConstraintViolation(
                            opCtx,
                            op.getNss(),
                            OplogConstraintViolationEnum::kDeleteOnMissingNs,
                            "delete",
                            opObj,
                            boost::none /* status */);
                    }

                    if (!isCapped) {
                        // This error is fatal when we are enforcing steady state constraints
                        // for non-capped collections.
                        uassert(collection ? ErrorCodes::NoSuchKey : ErrorCodes::NamespaceNotFound,
                                str::stream()
                                    << "Applied a delete which did not delete anything in "
                                       "steady state replication : "
                                    << redact(op.toBSONForLogging()),
                                !oplogApplicationEnforcesSteadyStateConstraints);
                    }
                }
                wuow.commit();
            });

            if (incrementOpsAppliedStats) {
                incrementOpsAppliedStats();
            }
            break;
        }
        case OpTypeEnum::kInsertGlobalIndexKey: {
            invariant(op.getUuid());

            Timestamp timestamp;
            if (assignOperationTimestamp) {
                timestamp = op.getTimestamp();
            }

            writeConflictRetryWithLimit(
                opCtx, "applyOps_insertGlobalIndexKey", collection->ns(), [&] {
                    WriteUnitOfWork wuow(opCtx);
                    if (timestamp != Timestamp::min()) {
                        uassertStatusOK(
                            shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(timestamp));
                    }

                    global_index::insertKey(
                        opCtx,
                        collectionAcquisition,
                        op.getObject().getObjectField(global_index::kOplogEntryIndexKeyFieldName),
                        op.getObject().getObjectField(global_index::kOplogEntryDocKeyFieldName));

                    wuow.commit();
                });
            break;
        }
        case OpTypeEnum::kDeleteGlobalIndexKey: {
            invariant(op.getUuid());

            Timestamp timestamp;
            if (assignOperationTimestamp) {
                timestamp = op.getTimestamp();
            }

            writeConflictRetryWithLimit(
                opCtx, "applyOps_deleteGlobalIndexKey", collection->ns(), [&] {
                    WriteUnitOfWork wuow(opCtx);
                    if (timestamp != Timestamp::min()) {
                        uassertStatusOK(
                            shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(timestamp));
                    }

                    global_index::deleteKey(
                        opCtx,
                        collectionAcquisition,
                        op.getObject().getObjectField(global_index::kOplogEntryIndexKeyFieldName),
                        op.getObject().getObjectField(global_index::kOplogEntryDocKeyFieldName));

                    wuow.commit();
                });
            break;
        }
        default: {
            // Commands are processed in applyCommand_inlock().
            invariant(false, str::stream() << "Unsupported opType " << OpType_serializer(opType));
        }
    }

    return Status::OK();
}

Status applyCommand_inlock(OperationContext* opCtx,
                           const ApplierOperation& op,
                           OplogApplication::Mode mode) {
    if (op->shouldLogAsDDLOperation() && !serverGlobalParams.quiet.load()) {
        LOGV2(7360110,
              "Applying DDL command oplog entry",
              "oplogEntry"_attr = op->toBSONForLogging(),
              "oplogApplicationMode"_attr = OplogApplication::modeToString(mode));
    } else {
        LOGV2_DEBUG(21255,
                    3,
                    "Applying command op",
                    "oplogEntry"_attr = redact(op->toBSONForLogging()),
                    "oplogApplicationMode"_attr = OplogApplication::modeToString(mode));
    }


    // Only commands are processed here.
    invariant(op->getOpType() == OpTypeEnum::kCommand);

    // Choose opCounters based on running on standalone/primary or secondary by checking
    // whether writes are replicated.
    OpCounters* opCounters = opCtx->writesAreReplicated() ? &globalOpCounters : &replOpCounters;
    opCounters->gotCommand();

    BSONObj o = op->getObject();

    const auto& nss = op->getNss();
    if (!nss.isValid()) {
        return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.toStringForErrorMsg()};
    }
    // The dbCheck batch might be operating on an older snapshot than what the secondary currently
    // has. Therefore, we should skip this check for dbCheck, as we know for sure it succeeded on
    // the primary, which means the collection must exist in the snapshot the dbcheck will run on.
    // Therefore, deferring the Point-in-Time (PIT) namespace existence check to dbCheck.
    if (strcmp(o.firstElementFieldName(), "dbCheck") != 0) {
        auto catalog = CollectionCatalog::get(opCtx);
        if (!catalog->lookupCollectionByNamespace(opCtx, nss) && catalog->lookupView(opCtx, nss)) {
            return {ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "applyOps not supported on view:"
                                  << nss.toStringForErrorMsg()};
        }
    }

    // The feature compatibility version in the server configuration collection cannot change during
    // initial sync. We do not attempt to parse the allowlisted ops because they do not have a
    // collection namespace. If we drop the 'admin' database we will also log a 'drop' oplog entry
    // for each collection dropped. 'applyOps' and 'commitTransaction' will try to apply each
    // individual operation, and those will be caught then if they are a problem. 'abortTransaction'
    // won't ever change the server configuration collection.
    std::vector<std::string> allowlistedOps{"dropDatabase",
                                            "applyOps",
                                            "dbCheck",
                                            "commitTransaction",
                                            "abortTransaction",
                                            "startIndexBuild",
                                            "commitIndexBuild",
                                            "abortIndexBuild"};
    if ((mode == OplogApplication::Mode::kInitialSync) &&
        (std::find(allowlistedOps.begin(), allowlistedOps.end(), o.firstElementFieldName()) ==
         allowlistedOps.end()) &&
        extractNs(nss.dbName(), o) == NamespaceString::kServerConfigurationNamespace) {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      str::stream() << "Applying command to feature compatibility version "
                                       "collection not supported in initial sync: "
                                    << redact(op->toBSONForLogging()));
    }

    // Parse optime from oplog entry unless we are applying this command in standalone or on a
    // primary (replicated writes enabled).
    OpTime opTime;
    if (!opCtx->writesAreReplicated()) {
        opTime = op->getOpTime();
    }

    const bool assignCommandTimestamp = [&] {
        if (opCtx->writesAreReplicated()) {
            // We do not assign timestamps on replicated writes since they will get their oplog
            // timestamp once they are logged.
            return false;
        }

        // Don't assign commit timestamp for transaction commands.
        if (op->shouldPrepare() ||
            op->getCommandType() == OplogEntry::CommandType::kCommitTransaction ||
            op->getCommandType() == OplogEntry::CommandType::kAbortTransaction) {
            return false;
        }

        if (mongo::feature_flags::gCreateCollectionInPreparedTransactions.isEnabled(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
            shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork()) {
            // Do not assign timestamps to non-replicated commands that have a wrapping
            // WriteUnitOfWork, as they will get the timestamp on that WUOW. Use cases include
            // secondary oplog application of prepared transactions.
            const auto cmdName = o.firstElementFieldNameStringData();
            invariant(cmdName == "create" || cmdName == "createIndexes");
            return false;
        }

        if (ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
            // The timestamps in the command oplog entries are always real timestamps from this
            // oplog and we should timestamp our writes with them.
            return true;
        } else {
            // Only assign timestamps on standalones during replication recovery when
            // started with 'recoverFromOplogAsStandalone'.
            return OplogApplication::inRecovering(mode);
        }
        MONGO_UNREACHABLE;
    }();
    invariant(!assignCommandTimestamp || !opTime.isNull(),
              str::stream() << "Oplog entry did not have 'ts' field when expected: "
                            << redact(op->toBSONForLogging()));

    const Timestamp writeTime = (assignCommandTimestamp ? opTime.getTimestamp() : Timestamp());

    bool done = false;
    while (!done) {
        auto opsMapIt = kOpsMap.find(o.firstElementFieldName());
        if (opsMapIt == kOpsMap.end()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid key '" << o.firstElementFieldName()
                                        << "' found in field 'o'");
        }
        const ApplyOpMetadata& curOpToApply = opsMapIt->second;

        Status status = [&] {
            try {
                // If 'writeTime' is not null, any writes in this scope will be given 'writeTime' as
                // their timestamp at commit.
                TimestampBlock tsBlock(opCtx, writeTime);
                return curOpToApply.applyFunc(opCtx, op, mode);
            } catch (const StorageUnavailableException&) {
                // Retriable error.
                throw;
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        }();

        switch (status.code()) {
            case ErrorCodes::BackgroundOperationInProgressForDatabase: {
                invariant(mode == OplogApplication::Mode::kInitialSync);

                // Aborting an index build involves writing to the catalog. This write needs to be
                // timestamped. It will be given 'writeTime' as the commit timestamp.
                TimestampBlock tsBlock(opCtx, writeTime);
                abortIndexBuilds(
                    opCtx, op->getCommandType(), nss, "Aborting index builds during initial sync");
                LOGV2_DEBUG(4665900,
                            1,
                            "Conflicting DDL operation encountered during initial sync; "
                            "aborting index build and retrying",
                            logAttrs(nss.dbName()));
                break;
            }
            case ErrorCodes::BackgroundOperationInProgressForNamespace: {
                Command* cmd = CommandHelpers::findCommand(opCtx, o.firstElement().fieldName());
                invariant(cmd);

                auto ns = cmd->parse(opCtx,
                                     OpMsgRequestBuilder::create(
                                         auth::ValidatedTenancyScope::get(opCtx), nss.dbName(), o))
                              ->ns();

                if (mode == OplogApplication::Mode::kInitialSync) {
                    // Aborting an index build involves writing to the catalog. This write needs to
                    // be timestamped. It will be given 'writeTime' as the commit timestamp.
                    TimestampBlock tsBlock(opCtx, writeTime);
                    abortIndexBuilds(opCtx,
                                     op->getCommandType(),
                                     ns,
                                     "Aborting index builds during initial sync");
                    LOGV2_DEBUG(4665901,
                                1,
                                "Conflicting DDL operation encountered during initial sync; "
                                "aborting index build and retrying",
                                logAttrs(ns));
                } else {
                    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

                    auto swUUID = op->getUuid();
                    if (!swUUID) {
                        LOGV2_ERROR(21261,
                                    "Failed command during oplog application. Expected a UUID",
                                    "command"_attr = redact(o),
                                    logAttrs(ns));
                    }

                    LOGV2_DEBUG(
                        7702500,
                        1,
                        "Waiting for index build(s) to complete on the namespace before retrying "
                        "the conflicting operation",
                        logAttrs(ns),
                        "oplogEntry"_attr = redact(op->toBSONForLogging()));

                    IndexBuildsCoordinator::get(opCtx)->awaitNoIndexBuildInProgressForCollection(
                        opCtx, swUUID.get());

                    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
                    opCtx->checkForInterrupt();

                    LOGV2_DEBUG(
                        51775,
                        1,
                        "Acceptable error during oplog application: background operation in "
                        "progress for namespace",
                        logAttrs(ns),
                        "oplogEntry"_attr = redact(op->toBSONForLogging()));
                }

                break;
            }
            default: {
                // Even when enforcing steady state constraints, we must allow IndexNotFound as
                // an index may not have been built on a secondary when a command dropping it
                // comes in.
                //
                // We can never enforce constraints on "dropDatabase" because a database is an
                // ephemeral entity that can be created or destroyed (if no collections exist)
                // without an oplog entry.
                if ((mode == OplogApplication::Mode::kSecondary &&
                     oplogApplicationEnforcesSteadyStateConstraints &&
                     status.code() != ErrorCodes::IndexNotFound &&
                     opsMapIt->first != "dropDatabase") ||
                    !curOpToApply.acceptableErrors.count(status.code())) {
                    LOGV2_ERROR(21262,
                                "Failed command during oplog application",
                                "command"_attr = redact(o),
                                logAttrs(nss.dbName()),
                                "error"_attr = status);
                    return status;
                }

                if (mode == OplogApplication::Mode::kSecondary &&
                    status.code() != ErrorCodes::IndexNotFound) {
                    const auto& opObj = redact(op->toBSONForLogging());
                    opCounters->gotAcceptableErrorInCommand();
                    logOplogConstraintViolation(
                        opCtx,
                        op->getNss(),
                        OplogConstraintViolationEnum::kAcceptableErrorInCommand,
                        "command",
                        opObj,
                        status);
                } else {
                    LOGV2_DEBUG(51776,
                                1,
                                "Acceptable error during oplog application",
                                logAttrs(nss.dbName()),
                                "error"_attr = status,
                                "oplogEntry"_attr = redact(op->toBSONForLogging()));
                }
                [[fallthrough]];
            }
            case ErrorCodes::OK:
                done = true;
                break;
        }
    }

    AuthorizationManager::get(opCtx->getService())->logOp(opCtx, "c", nss, o, nullptr);
    return Status::OK();
}

void setNewTimestamp(ServiceContext* service, const Timestamp& newTime) {
    LocalOplogInfo::get(service)->setNewTimestamp(service, newTime);
}

void initTimestampFromOplog(OperationContext* opCtx, const NamespaceString& oplogNss) {
    DBDirectClient c(opCtx);
    static const BSONObj reverseNaturalObj = BSON("$natural" << -1);
    FindCommandRequest findCmd{oplogNss};
    findCmd.setSort(reverseNaturalObj);
    BSONObj lastOp =
        c.findOne(std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryPreferred});

    if (!lastOp.isEmpty()) {
        LOGV2_DEBUG(21256, 1, "replSet setting last Timestamp");
        const OpTime opTime = fassert(28696, OpTime::parseFromOplogEntry(lastOp));
        setNewTimestamp(opCtx->getServiceContext(), opTime.getTimestamp());
    }
}

void clearLocalOplogPtr(ServiceContext* service) {
    LocalOplogInfo::get(service)->resetCollection();
}

void acquireOplogCollectionForLogging(OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, NamespaceString::kRsOplogNamespace, MODE_IX);
    LocalOplogInfo::get(opCtx)->setCollection(autoColl.getCollection().get());
}

void establishOplogCollectionForLogging(OperationContext* opCtx, const Collection* oplog) {
    invariant(shard_role_details::getLocker(opCtx)->isW());
    invariant(oplog);
    LocalOplogInfo::get(opCtx)->setCollection(oplog);
}

void signalOplogWaiters() {
    const auto& oplog = LocalOplogInfo::get(getGlobalServiceContext())->getCollection();
    if (oplog) {
        oplog->getRecordStore()->getCappedInsertNotifier()->notifyAll();
    }
}

}  // namespace repl
}  // namespace mongo
