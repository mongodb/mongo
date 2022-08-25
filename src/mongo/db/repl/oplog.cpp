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

#include <deque>
#include <fmt/format.h>
#include <memory>
#include <set>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/capped_collection_maintenance.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/file.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace repl {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(addDestinedRecipient);
MONGO_FAIL_POINT_DEFINE(sleepBetweenInsertOpTimeGenerationAndLogOp);

// Failpoint to block after a write and its oplog entry have been written to the storage engine and
// are visible, but before we have advanced 'lastApplied' for the write.
MONGO_FAIL_POINT_DEFINE(hangBeforeLogOpAdvancesLastApplied);

void abortIndexBuilds(OperationContext* opCtx,
                      const OplogEntry::CommandType& commandType,
                      const NamespaceString& nss,
                      const std::string& reason) {
    auto indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
    if (commandType == OplogEntry::CommandType::kDropDatabase) {
        indexBuildsCoordinator->abortDatabaseIndexBuilds(opCtx, nss.db(), reason);
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
    invariant(opCtx->lockState()->isWriteLocked());

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

}  // namespace

ApplyImportCollectionFn applyImportCollection = applyImportCollectionDefault;

void registerApplyImportCollectionFn(ApplyImportCollectionFn func) {
    applyImportCollection = func;
}

void createIndexForApplyOps(OperationContext* opCtx,
                            const BSONObj& indexSpec,
                            const NamespaceString& indexNss,
                            OplogApplication::Mode mode) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(indexNss, MODE_X));

    // Check if collection exists.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, indexNss.dbName());
    auto indexCollection =
        db ? CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, indexNss) : nullptr;
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to create index due to missing collection: " << indexNss.ns(),
            indexCollection);

    OpCounters* opCounters = opCtx->writesAreReplicated() ? &globalOpCounters : &replOpCounters;
    opCounters->gotInsert();
    if (opCtx->writesAreReplicated()) {
        ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
            opCtx->getWriteConcern());
    }

    // Check for conflict with two-phase index builds during initial sync. It is possible that
    // this index may have been dropped and recreated after inserting documents into the collection.
    auto indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
    if (OplogApplication::Mode::kInitialSync == mode) {
        auto normalSpecs =
            indexBuildsCoordinator->normalizeIndexSpecs(opCtx, indexCollection, {indexSpec});
        invariant(1U == normalSpecs.size(),
                  str::stream() << "Unexpected result from normalizeIndexSpecs - ns: " << indexNss
                                << "; uuid: " << indexCollection->uuid()
                                << "; original index spec: " << indexSpec
                                << "; normalized index specs: "
                                << BSON("normalSpecs" << normalSpecs));
        auto indexCatalog = indexCollection->getIndexCatalog();
        auto prepareSpecResult =
            indexCatalog->prepareSpecForCreate(opCtx, indexCollection, normalSpecs[0], {});
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
                            << indexNss << "; uuid: " << indexCollection->uuid()
                            << "; original index spec: " << indexSpec);
    const auto constraints = IndexBuildsManager::IndexConstraints::kRelax;

    // Run single-phase builds synchronously with oplog batch application. This enables them to
    // stop using ghost timestamps. Single phase builds are only used for empty collections, and
    // to rebuild indexes admin.system collections. See SERVER-47439.
    IndexBuildsCoordinator::updateCurOpOpDescription(opCtx, indexNss, {indexSpec});
    auto collUUID = indexCollection->uuid();
    auto fromMigrate = false;
    indexBuildsCoordinator->createIndex(opCtx, collUUID, indexSpec, constraints, fromMigrate);

    opCtx->recoveryUnit()->abandonSnapshot();
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
                            const StringData& invalidatedReason,
                            bool* upsertConfigImage) {
    // In practice, this lock acquisition on kConfigImagesNamespace cannot block. The only time a
    // stronger lock acquisition is taken on this namespace is during step up to create the
    // collection.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    AutoGetCollection autoColl(opCtx, NamespaceString::kConfigImagesNamespace, LockMode::MODE_IX);
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
    request.setQuery(
        BSON("_id" << imageEntry.get_id().toBSON() << "ts" << BSON("$lte" << imageEntry.getTs())));
    request.setUpsert(*upsertConfigImage);
    request.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(imageEntry.toBSON()));
    request.setFromOplogApplication(true);
    try {
        // This code path can also be hit by things such as `applyOps` and tenant migrations.
        ::mongo::update(opCtx, autoColl.getDb(), request);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // We can get a duplicate key when two upserts race on inserting a document.
        *upsertConfigImage = false;
        // This write conflict is always retried internally and never exposed to the user.
        throwWriteConflictException(
            "DuplicateKey error when inserting a document into the pre-images collection.");
    }
}

/* we write to local.oplog.rs:
     { ts : ..., h: ..., v: ..., op: ..., etc }
   ts: an OpTime timestamp
   h: hash
   v: version
   op:
    "i" insert
    "u" update
    "d" delete
    "c" db cmd
    "n" no op
*/


/*
 * records - a vector of oplog records to be written.
 * timestamps - a vector of respective Timestamp objects for each oplog record.
 * oplogCollection - collection to be written to.
 * finalOpTime - the OpTime of the last oplog record.
 * wallTime - the wall clock time of the last oplog record.
 */
void _logOpsInner(OperationContext* opCtx,
                  const NamespaceString& nss,
                  std::vector<Record>* records,
                  const std::vector<Timestamp>& timestamps,
                  const CollectionPtr& oplogCollection,
                  OpTime finalOpTime,
                  Date_t wallTime,
                  bool isAbortIndexBuild) {
    auto replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->getReplicationMode() == ReplicationCoordinator::modeReplSet &&
        !replCoord->canAcceptWritesFor(opCtx, nss)) {
        str::stream ss;
        ss << "logOp() but can't accept write to collection " << nss;
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
        tenant_migration_access_blocker::checkIfCanWriteOrThrow(opCtx, nss.db(), timestamps.back());
    }

    Status result = insertDocumentsForOplog(opCtx, oplogCollection, records, timestamps);
    if (!result.isOK()) {
        LOGV2_FATAL(17322,
                    "write to oplog failed: {error}",
                    "Write to oplog failed",
                    "error"_attr = result.toString());
    }

    // Insert the oplog records to the respective tenants change collections.
    if (ChangeStreamChangeCollectionManager::isChangeCollectionsModeActive()) {
        ChangeStreamChangeCollectionManager::get(opCtx).insertDocumentsToChangeCollection(
            opCtx, *records, timestamps);
    }

    // Set replCoord last optime only after we're sure the WUOW didn't abort and roll back.
    opCtx->recoveryUnit()->onCommit(
        [opCtx, replCoord, finalOpTime, wallTime](boost::optional<Timestamp> commitTime) {
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
            replCoord->setMyLastAppliedOpTimeAndWallTimeForward({finalOpTime, wallTime});

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
                              << oplogEntry->getNss().ns(),
                oplogEntry->getStatementIds().empty());
        return {};
    }
    // If this oplog entry is from a tenant migration, include the tenant migration
    // UUID.
    const auto& recipientInfo = tenantMigrationInfo(opCtx);
    if (recipientInfo) {
        oplogEntry->setFromTenantMigration(recipientInfo->uuid);
    }

    // TODO SERVER-51301 to remove this block.
    if (oplogEntry->getOpType() == repl::OpTypeEnum::kNoop) {
        opCtx->recoveryUnit()->ignoreAllMultiTimestampConstraints();
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
    _logOpsInner(opCtx,
                 oplogEntry->getNss(),
                 &records,
                 timestamps,
                 oplog,
                 slot,
                 wallClockTime,
                 isAbortIndexBuild);
    wuow.commit();
    return slot;
}

std::vector<OpTime> logInsertOps(
    OperationContext* opCtx,
    MutableOplogEntry* oplogEntryTemplate,
    std::vector<InsertStatement>::const_iterator begin,
    std::vector<InsertStatement>::const_iterator end,
    std::function<boost::optional<ShardId>(const BSONObj& doc)> getDestinedRecipientFn) {
    invariant(begin != end);
    oplogEntryTemplate->setOpType(repl::OpTypeEnum::kInsert);
    // If this oplog entry is from a tenant migration, include the tenant migration
    // UUID.
    const auto& recipientInfo = tenantMigrationInfo(opCtx);
    if (recipientInfo) {
        oplogEntryTemplate->setFromTenantMigration(recipientInfo->uuid);
    }

    auto nss = oplogEntryTemplate->getNss();
    auto replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->isOplogDisabledFor(opCtx, nss)) {
        invariant(!begin->stmtIds.empty());
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "retryable writes is not supported for unreplicated ns: "
                              << nss.ns(),
                begin->stmtIds.front() == kUninitializedStmtId);
        return {};
    }

    const size_t count = end - begin;

    // Use OplogAccessMode::kLogOp to avoid recursive locking.
    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kLogOp);
    auto oplogInfo = oplogWrite.getOplogInfo();

    write_stage_common::PreWriteFilter preWriteFilter(opCtx, nss);

    WriteUnitOfWork wuow(opCtx);

    std::vector<OpTime> opTimes(count);
    std::vector<Timestamp> timestamps(count);
    std::vector<BSONObj> bsonOplogEntries(count);
    std::vector<Record> records(count);
    for (size_t i = 0; i < count; i++) {
        // Make a copy from the template for each insert oplog entry.
        MutableOplogEntry oplogEntry = *oplogEntryTemplate;
        // Make a mutable copy.
        auto insertStatementOplogSlot = begin[i].oplogSlot;
        // Fetch optime now, if not already fetched.
        if (insertStatementOplogSlot.isNull()) {
            insertStatementOplogSlot = oplogInfo->getNextOpTimes(opCtx, 1U)[0];
        }
        const auto docKey = getDocumentKey(opCtx, nss, begin[i].doc).getShardKeyAndId();
        oplogEntry.setObject(begin[i].doc);
        oplogEntry.setObject2(docKey);
        oplogEntry.setOpTime(insertStatementOplogSlot);
        oplogEntry.setDestinedRecipient(getDestinedRecipientFn(begin[i].doc));
        addDestinedRecipient.execute([&](const BSONObj& data) {
            auto recipient = data["destinedRecipient"].String();
            oplogEntry.setDestinedRecipient(boost::make_optional<ShardId>({recipient}));
        });

        OplogLink oplogLink;
        if (i > 0)
            oplogLink.prevOpTime = opTimes[i - 1];

        // Direct inserts to shards of orphan documents should not generate change stream events.
        if (!oplogEntry.getFromMigrate().value_or(false) &&
            !OperationShardingState::isComingFromRouter(opCtx) &&
            preWriteFilter.computeAction(Document(begin[i].doc)) ==
                write_stage_common::PreWriteFilter::Action::kWriteAsFromMigrate) {
            LOGV2_DEBUG(6258100,
                        3,
                        "Marking insert operation of orphan document with the 'fromMigrate' flag "
                        "to prevent a wrong change stream event",
                        "namespace"_attr = nss,
                        "document"_attr = begin[i].doc);

            oplogEntry.setFromMigrate(true);
        }
        appendOplogEntryChainInfo(opCtx, &oplogEntry, &oplogLink, begin[i].stmtIds);

        opTimes[i] = insertStatementOplogSlot;
        timestamps[i] = insertStatementOplogSlot.getTimestamp();
        bsonOplogEntries[i] = oplogEntry.toBSON();
        // The storage engine will assign the RecordId based on the "ts" field of the oplog entry,
        // see record_id_helpers::extractKey.
        records[i] = Record{
            RecordId(), RecordData(bsonOplogEntries[i].objdata(), bsonOplogEntries[i].objsize())};
    }

    sleepBetweenInsertOpTimeGenerationAndLogOp.execute([&](const BSONObj& data) {
        auto numMillis = data["waitForMillis"].numberInt();
        LOGV2(21244,
              "Sleeping for {sleepMillis}ms after receiving {numOpTimesReceived} optimes from "
              "{firstOpTime} to "
              "{lastOpTime}",
              "Sleeping due to sleepBetweenInsertOpTimeGenerationAndLogOp failpoint",
              "sleepMillis"_attr = numMillis,
              "numOpTimesReceived"_attr = count,
              "firstOpTime"_attr = opTimes.front(),
              "lastOpTime"_attr = opTimes.back());
        sleepmillis(numMillis);
    });

    invariant(!opTimes.empty());
    auto lastOpTime = opTimes.back();
    invariant(!lastOpTime.isNull());
    const auto& oplog = oplogInfo->getCollection();
    auto wallClockTime = oplogEntryTemplate->getWallClockTime();
    const bool isAbortIndexBuild = false;
    _logOpsInner(
        opCtx, nss, &records, timestamps, oplog, lastOpTime, wallClockTime, isAbortIndexBuild);
    wuow.commit();
    return opTimes;
}

void appendOplogEntryChainInfo(OperationContext* opCtx,
                               MutableOplogEntry* oplogEntry,
                               OplogLink* oplogLink,
                               const std::vector<StmtId>& stmtIds) {
    invariant(!stmtIds.empty());

    // We sometimes have a pre-image no-op entry even for normal non-retryable writes
    // if recordPreImages is enabled on the collection.
    if (!oplogLink->preImageOpTime.isNull()) {
        oplogEntry->setPreImageOpTime(oplogLink->preImageOpTime);
    }

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
    oplogEntry->setStatementIds(stmtIds);
    if (oplogLink->prevOpTime.isNull()) {
        oplogLink->prevOpTime = txnParticipant.getLastWriteOpTime();
    }
    oplogEntry->setPrevWriteOpTimeInTransaction(oplogLink->prevOpTime);
    if (!oplogLink->postImageOpTime.isNull()) {
        oplogEntry->setPostImageOpTime(oplogLink->postImageOpTime);
    }
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
        LOGV2_DEBUG(21245,
                    3,
                    "32bit system; choosing {oplogSizeBytes} bytes oplog",
                    "Choosing oplog size for 32bit system",
                    "oplogSizeBytes"_attr = sz);
        return sz;
    }
    // First choose a minimum size.

#if defined(__APPLE__)
    // typically these are desktops (dev machines), so keep it smallish
    const auto sz = 192 * 1024 * 1024;
    LOGV2_DEBUG(21246,
                3,
                "Apple system; choosing {oplogSizeBytes} bytes oplog",
                "Choosing oplog size for Apple system",
                "oplogSizeBytes"_attr = sz);
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
                    "Ephemeral storage system; lowerBound: {lowerBoundBytes} bytes, "
                    "{totalMemoryBytes} bytes total memory",
                    "Ephemeral storage system",
                    "lowerBoundBytes"_attr = lowerBound,
                    "totalMemoryBytes"_attr = bytes);
    } else {
        // disk: 990MB minimum size
        lowerBound = 990LL * 1024 * 1024;
        bytes = File::freeSpace(storageGlobalParams.dbpath);  //-1 if call not supported.
        LOGV2_DEBUG(21248,
                    3,
                    "Disk storage system; lowerBound: {lowerBoundBytes} bytes, {freeSpaceBytes} "
                    "bytes free space "
                    "on device",
                    "Disk storage system",
                    "lowerBoundBytes"_attr = lowerBound,
                    "freeSpaceBytes"_attr = bytes);
    }
    long long fivePct = static_cast<long long>(bytes * 0.05);
    auto sz = std::max(fivePct, lowerBound);
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
    CollectionPtr collection =
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

    LOGV2(21251,
          "creating replication oplog of size: {oplogSizeMegabytes}MB...",
          "Creating replication oplog",
          "oplogSizeMB"_attr = (int)(sz / (1024 * 1024)));

    CollectionOptions options;
    options.capped = true;
    options.cappedSize = sz;
    options.autoIndexId = CollectionOptions::NO;

    writeConflictRetry(opCtx, "createCollection", oplogCollectionName.ns(), [&] {
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
    const auto isReplSet = ReplicationCoordinator::get(opCtx)->getReplicationMode() ==
        ReplicationCoordinator::modeReplSet;
    createOplog(opCtx, NamespaceString::kRsOplogNamespace, isReplSet);
}

std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count) {
    return LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, count);
}

// -------------------------------------

namespace {
NamespaceString extractNs(StringData db, const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    uassert(40073,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    StringData coll = first.valueStringData();
    uassert(28635, "no collection name specified", !coll.empty());
    return NamespaceString(db, coll);
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
    return ui ? extractNsFromUUID(opCtx, ui.value()) : extractNs(ns.db(), cmd);
}

using OpApplyFn = std::function<Status(
    OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode)>;

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
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& ui = entry.getUuid();
          const auto& cmd = entry.getObject();
          const NamespaceString nss(extractNs(entry.getNss().db(), cmd));

          const auto& migrationId = entry.getFromTenantMigration();
          if (migrationId) {
              tenantMigrationInfo(opCtx) = boost::optional<TenantMigrationInfo>(migrationId);
          }

          // Mode SECONDARY steady state replication should not allow create collection to rename an
          // existing collection out of the way. This leaves a collection orphaned and is a bug.
          // Renaming temporarily out of the way is only allowed for oplog replay, where we expect
          // any temporarily renamed aside collections to be sorted out by the time replay is
          // complete.
          const bool allowRenameOutOfTheWay = (mode != repl::OplogApplication::Mode::kSecondary);

          Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
          if (auto idIndexElem = cmd["idIndex"]) {
              // Remove "idIndex" field from command.
              auto cmdWithoutIdIndex = cmd.removeField("idIndex");
              return createCollectionForApplyOps(opCtx,
                                                 nss.db().toString(),
                                                 ui,
                                                 cmdWithoutIdIndex,
                                                 allowRenameOutOfTheWay,
                                                 idIndexElem.Obj());
          }

          // Collections clustered by _id do not need _id indexes.
          if (auto clusteredElem = cmd["clusteredIndex"]) {
              return createCollectionForApplyOps(
                  opCtx, nss.db().toString(), ui, cmd, allowRenameOutOfTheWay, boost::none);
          }

          // No _id index spec was provided, so we should build a v:1 _id index.
          BSONObjBuilder idIndexSpecBuilder;
          idIndexSpecBuilder.append(IndexDescriptor::kIndexVersionFieldName,
                                    static_cast<int>(IndexVersion::kV1));
          idIndexSpecBuilder.append(IndexDescriptor::kIndexNameFieldName, "_id_");
          idIndexSpecBuilder.append(IndexDescriptor::kKeyPatternFieldName, BSON("_id" << 1));
          return createCollectionForApplyOps(opCtx,
                                             nss.db().toString(),
                                             ui,
                                             cmd,
                                             allowRenameOutOfTheWay,
                                             idIndexSpecBuilder.done());
      },
      {ErrorCodes::NamespaceExists}}},
    {"createIndexes",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& cmd = entry.getObject();
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
          Lock::CollectionLock collLock(opCtx, nss, MODE_X);
          createIndexForApplyOps(opCtx, indexSpec, nss, mode);
          return Status::OK();
      },
      {ErrorCodes::IndexAlreadyExists,
       ErrorCodes::IndexBuildAlreadyInProgress,
       ErrorCodes::NamespaceNotFound}}},
    {"startIndexBuild",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          if (OplogApplication::Mode::kApplyOpsCmd == mode) {
              return {ErrorCodes::CommandNotSupported,
                      "The startIndexBuild operation is not supported in applyOps mode"};
          }

          auto swOplogEntry = IndexBuildOplogEntry::parse(entry);
          if (!swOplogEntry.isOK()) {
              return swOplogEntry.getStatus().withContext(
                  "Error parsing 'startIndexBuild' oplog entry");
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
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          if (OplogApplication::Mode::kApplyOpsCmd == mode) {
              return {ErrorCodes::CommandNotSupported,
                      "The commitIndexBuild operation is not supported in applyOps mode"};
          }

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
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          if (OplogApplication::Mode::kApplyOpsCmd == mode) {
              return {ErrorCodes::CommandNotSupported,
                      "The abortIndexBuild operation is not supported in applyOps mode"};
          }

          auto swOplogEntry = IndexBuildOplogEntry::parse(entry);
          if (!swOplogEntry.isOK()) {
              return swOplogEntry.getStatus().withContext(
                  "Error parsing 'abortIndexBuild' oplog entry");
          }
          IndexBuildsCoordinator::get(opCtx)->applyAbortIndexBuild(opCtx, swOplogEntry.getValue());
          return Status::OK();
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"collMod",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& cmd = entry.getObject();
          auto opMsg = OpMsgRequest::fromDBAndBody(entry.getNss().db(), cmd);
          auto collModCmd = CollMod::parse(IDLParserContext("collModOplogEntry"), opMsg);
          const auto nssOrUUID([&collModCmd, &entry, mode]() -> NamespaceStringOrUUID {
              // Oplog entries from secondary oplog application will allways have the Uuid set and
              // it is only invocations of applyOps directly that may omit it
              if (!entry.getUuid()) {
                  invariant(mode == OplogApplication::Mode::kApplyOpsCmd);
                  return collModCmd.getNamespace();
              }

              return {collModCmd.getDbName().toString(), *entry.getUuid()};
          }());
          return processCollModCommandForApplyOps(opCtx, nssOrUUID, collModCmd, mode);
      },
      {ErrorCodes::IndexNotFound, ErrorCodes::NamespaceNotFound}}},
    {"dbCheck", {dbCheckOplogCommand, {}}},
    {"dropDatabase",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          return dropDatabaseForApplyOps(opCtx, entry.getNss().dbName());
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"drop",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& cmd = entry.getObject();
          auto nss = extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd);
          if (nss.isDropPendingNamespace()) {
              LOGV2(21253,
                    "applyCommand: {namespace} : collection is already in a drop-pending state: "
                    "ignoring collection drop: {command}",
                    "applyCommand: collection is already in a drop-pending state, ignoring "
                    "collection drop",
                    "namespace"_attr = nss,
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
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& cmd = entry.getObject();
          return dropIndexesForApplyOps(
              opCtx, extractNsFromUUID(opCtx, entry.getUuid().value()), cmd);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"deleteIndexes",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& cmd = entry.getObject();
          return dropIndexesForApplyOps(
              opCtx, extractNsFromUUID(opCtx, entry.getUuid().value()), cmd);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndex",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& cmd = entry.getObject();
          return dropIndexesForApplyOps(
              opCtx, extractNsFromUUID(opCtx, entry.getUuid().value()), cmd);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndexes",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& cmd = entry.getObject();
          return dropIndexesForApplyOps(
              opCtx, extractNsFromUUID(opCtx, entry.getUuid().value()), cmd);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"renameCollection",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          // Parse optime from oplog entry unless we are applying this command in standalone or on a
          // primary (replicated writes enabled).
          OpTime opTime;
          if (!opCtx->writesAreReplicated()) {
              opTime = entry.getOpTime();
          }
          return renameCollectionForApplyOps(
              opCtx, entry.getNss().db().toString(), entry.getUuid(), entry.getObject(), opTime);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::NamespaceExists}}},
    {"importCollection",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          auto importEntry = mongo::ImportCollectionOplogEntry::parse(
              IDLParserContext("importCollectionOplogEntry"), entry.getObject());
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
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
         return entry.shouldPrepare() ? applyPrepareTransaction(opCtx, entry, mode)
                                      : applyApplyOpsOplogEntry(opCtx, entry, mode);
     }}},
    {"convertToCapped",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          const auto& cmd = entry.getObject();
          convertToCapped(opCtx,
                          extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), cmd),
                          cmd["size"].number());
          return Status::OK();
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"emptycapped",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
          return emptyCapped(
              opCtx,
              extractNsFromUUIDorNs(opCtx, entry.getNss(), entry.getUuid(), entry.getObject()));
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"commitTransaction",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
         return applyCommitTransaction(opCtx, entry, mode);
     }}},
    {"abortTransaction",
     {[](OperationContext* opCtx, const OplogEntry& entry, OplogApplication::Mode mode) -> Status {
         return applyAbortTransaction(opCtx, entry, mode);
     }}},
};

// Writes a change stream pre-image 'preImage' associated with oplog entry 'oplogEntry' and a write
// operation to collection 'collection' with "applyOpsIndex" 0.
void writeChangeStreamPreImage(OperationContext* opCtx,
                               const CollectionPtr& collection,
                               const mongo::repl::OplogEntry& oplogEntry,
                               const BSONObj& preImage) {
    ChangeStreamPreImageId preImageId{collection->uuid(),
                                      oplogEntry.getTimestampForPreImage(),
                                      static_cast<int64_t>(oplogEntry.getApplyOpsIndex())};
    ChangeStreamPreImage preImageDocument{
        std::move(preImageId), oplogEntry.getWallClockTimeForPreImage(), preImage};

    // TODO SERVER-66643 Pass tenant id to the pre-images collection if running in the serverless.
    ChangeStreamPreImagesCollectionManager::insertPreImage(
        opCtx, /* tenantId */ boost::none, preImageDocument);
}
}  // namespace

constexpr StringData OplogApplication::kInitialSyncOplogApplicationMode;
constexpr StringData OplogApplication::kRecoveringOplogApplicationMode;
constexpr StringData OplogApplication::kSecondaryOplogApplicationMode;
constexpr StringData OplogApplication::kApplyOpsCmdOplogApplicationMode;

StringData OplogApplication::modeToString(OplogApplication::Mode mode) {
    switch (mode) {
        case OplogApplication::Mode::kInitialSync:
            return OplogApplication::kInitialSyncOplogApplicationMode;
        case OplogApplication::Mode::kRecovering:
            return OplogApplication::kRecoveringOplogApplicationMode;
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
        return OplogApplication::Mode::kRecovering;
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

// @return failure status if an update should have happened and the document DNE.
// See replset initial sync code.
Status applyOperation_inlock(OperationContext* opCtx,
                             Database* db,
                             const OplogEntryOrGroupedInserts& opOrGroupedInserts,
                             bool alwaysUpsert,
                             OplogApplication::Mode mode,
                             const bool isDataConsistent,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats) {
    // Get the single oplog entry to be applied or the first oplog entry of grouped inserts.
    auto op = opOrGroupedInserts.getOp();
    LOGV2_DEBUG(21254,
                3,
                "applying op (or grouped inserts): {op}, oplog application mode: "
                "{oplogApplicationMode}",
                "Applying op (or grouped inserts)",
                "op"_attr = redact(opOrGroupedInserts.toBSON()),
                "oplogApplicationMode"_attr = OplogApplication::modeToString(mode));

    // Choose opCounters based on running on standalone/primary or secondary by checking
    // whether writes are replicated. Atomic applyOps command is an exception, which runs
    // on primary/standalone but disables write replication.
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

    NamespaceString requestNss;
    CollectionPtr collection = nullptr;
    if (auto uuid = op.getUuid()) {
        auto catalog = CollectionCatalog::get(opCtx);
        collection = catalog->lookupCollectionByUUID(opCtx, uuid.value());
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply operation due to missing collection ("
                              << uuid.value() << "): " << redact(opOrGroupedInserts.toBSON()),
                collection);
        requestNss = collection->ns();
        dassert(opCtx->lockState()->isCollectionLockedForMode(requestNss, MODE_IX));
    } else {
        requestNss = op.getNss();
        invariant(requestNss.coll().size());
        dassert(opCtx->lockState()->isCollectionLockedForMode(requestNss, MODE_IX),
                requestNss.ns());
        collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, requestNss);
    }

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

    const IndexCatalog* indexCatalog =
        collection == nullptr ? nullptr : collection->getIndexCatalog();
    const bool haveWrappingWriteUnitOfWork = opCtx->lockState()->inAWriteUnitOfWork();
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "applyOps not supported on view: " << requestNss.ns(),
            collection || !CollectionCatalog::get(opCtx)->lookupView(opCtx, requestNss));

    // Decide whether to timestamp the write with the 'ts' field found in the operation. In general,
    // we do this for secondary oplog application, but there are some exceptions.
    const bool assignOperationTimestamp = [opCtx, haveWrappingWriteUnitOfWork, mode] {
        const auto replMode = ReplicationCoordinator::get(opCtx)->getReplicationMode();
        if (opCtx->writesAreReplicated()) {
            // We do not assign timestamps on replicated writes since they will get their oplog
            // timestamp once they are logged. The operation may contain a timestamp if it is part
            // of a non-atomic applyOps command, but we ignore it so that we don't violate oplog
            // ordering.
            return false;
        } else if (haveWrappingWriteUnitOfWork) {
            // We do not assign timestamps to non-replicated writes that have a wrapping
            // WriteUnitOfWork, as they will get the timestamp on that WUOW. Use cases include: (1)
            // Atomic applyOps (used by sharding). (2) Secondary oplog application of prepared
            // transactions.
            return false;
        } else {
            switch (replMode) {
                case ReplicationCoordinator::modeReplSet: {
                    // Secondary oplog application not in a WUOW uses the timestamp in the operation
                    // document.
                    return true;
                }
                case ReplicationCoordinator::modeNone: {
                    // Only assign timestamps on standalones during replication recovery when
                    // started with the 'recoverFromOplogAsStandalone' flag.
                    return mode == OplogApplication::Mode::kRecovering;
                }
            }
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
            (mode == OplogApplication::Mode::kRecovering ||
             mode == OplogApplication::Mode::kSecondary) &&
            !op.getFromMigrate().get_value_or(false) &&
            !requestNss.isTemporaryReshardingCollection();
    };

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
                    for (const auto iOp : insertOps) {
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
                    for (const auto iOp : insertOps) {
                        insertObjs.emplace_back(
                            iOp->getObject(), slotIter->getTimestamp(), slotIter->getTerm());
                        slotIter++;
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
                for (auto entry : insertObjs) {
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
                            opCounters->gotInsertOnExistingDoc();
                            if (oplogApplicationEnforcesSteadyStateConstraints) {
                                return status;
                            }
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

                    const StringData ns = op.getNss().ns();
                    writeConflictRetry(opCtx, "applyOps_upsert", ns, [&] {
                        WriteUnitOfWork wuow(opCtx);
                        // If this is an atomic applyOps (i.e: `haveWrappingWriteUnitOfWork` is
                        // true), do not timestamp the write.
                        if (assignOperationTimestamp && timestamp != Timestamp::min()) {
                            uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(timestamp));
                        }

                        UpdateResult res = update(opCtx, db, request);
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
            // enable some optimizations in diff application. In some cases, during tenant
            // migration, we can for some reason generate entries for timeseries bucket collections
            // which still rely on the idempotency guarantee, which then means we shouldn't apply
            // these optimizations.
            write_ops::UpdateModification::DiffOptions options;
            if (mode == OplogApplication::Mode::kSecondary && collection->getTimeseriesOptions() &&
                !op.getFromTenantMigration()) {
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

            const StringData ns = op.getNss().ns();
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
            // long as the last write persists. To accomplish this we update
            // `config.image_collection` entries with an upsert. The query predicate is `{_id:
            // <lsid>, ts $lt <oplogEntry.ts>}`. This can result in a WriteConflictException when
            // two writers are concurrently updating/inserting the same document.
            //
            // However, when an upsert turns into an insert, a writer can also observe a
            // DuplicateKeyException as its `ts` clause can hide the document from being
            // updated. Following up the failed update with an insert turns into a
            // DuplicateKeyException. This is safe, but to break an infinite loop, we retry the
            // operation with a regular update as opposed to an upsert. We're guaranteed to not need
            // to insert a document. We only have to make sure we didn't race with an insert that
            // won, but with an earlier `ts`.
            bool upsertConfigImage = true;
            auto status = writeConflictRetry(opCtx, "applyOps_update", ns, [&] {
                WriteUnitOfWork wuow(opCtx);
                if (timestamp != Timestamp::min()) {
                    uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(timestamp));
                }

                if (recordChangeStreamPreImage && request.shouldReturnNewDocs()) {
                    // Load the document version before update to be used as the change stream
                    // pre-image since the update operation will load the new version of the
                    // document.
                    invariant(op.getObject2());
                    auto&& documentId = *op.getObject2();
                    auto documentFound = Helpers::findById(
                        opCtx, collection->ns().ns(), documentId, changeStreamPreImage);
                    invariant(documentFound);
                }

                UpdateResult ur = update(opCtx, db, request);
                if (ur.numMatched == 0 && ur.upsertedId.isEmpty()) {
                    if (collection && collection->isCapped() &&
                        mode == OplogApplication::Mode::kSecondary) {
                        // We can't assume there was a problem when the collection is capped,
                        // because the item may have been deleted by the cappedDeleter.  This only
                        // matters for steady-state mode, because all errors on missing updates are
                        // ignored at a higher level for recovery and initial sync.
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
                        // failure. Note that adds some overhead for this extra check in some cases,
                        // such as an updateCriteria of the form
                        // { _id:..., { x : {$size:...} }
                        // thus this is not ideal.
                        if (collection == nullptr ||
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

                        // Otherwise, it's present; zero objects were updated because of additional
                        // specifiers in the query for idempotence
                    } else {
                        // this could happen benignly on an oplog duplicate replay of an upsert
                        // (because we are idempotent), if a regular non-mod update fails the item
                        // is (presumably) missing.
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
                    LOGV2_WARNING(2170001,
                                  "update needed to be converted to upsert",
                                  "op"_attr = redact(op.toBSONForLogging()));
                    opCounters->gotUpdateOnMissingDoc();

                    // We shouldn't be doing upserts in secondary mode when enforcing steady state
                    // constraints.
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
                                           getInvalidatingReason(mode, isDataConsistent),
                                           &upsertConfigImage);
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

            const StringData ns = op.getNss().ns();
            bool upsertConfigImage = true;
            writeConflictRetry(opCtx, "applyOps_delete", ns, [&] {
                WriteUnitOfWork wuow(opCtx);
                if (timestamp != Timestamp::min()) {
                    uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(timestamp));
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
                    // Request loading of the document version before delete operation to be used as
                    // change stream pre-image.
                    request.setReturnDeleted(true);
                }

                DeleteResult result = deleteObject(opCtx, collection, request);
                if (op.getNeedsRetryImage()) {
                    // Even if `result.nDeleted` is 0, we want to perform a write to the
                    // imageCollection to advance the txnNumber/ts and invalidate the image. This
                    // isn't strictly necessary for correctness -- the `config.transactions` table
                    // is responsible for whether to retry. The motivation here is to simply reduce
                    // the number of states related documents in the two collections can be in.
                    writeToImageCollection(opCtx,
                                           op.getSessionId().value(),
                                           op.getTxnNumber().value(),
                                           op.getApplyOpsTimestamp().value_or(op.getTimestamp()),
                                           repl::RetryImageEnum::kPreImage,
                                           result.requestedPreImage.value_or(BSONObj()),
                                           getInvalidatingReason(mode, isDataConsistent),
                                           &upsertConfigImage);
                }

                if (recordChangeStreamPreImage) {
                    invariant(result.requestedPreImage);
                    writeChangeStreamPreImage(opCtx, collection, op, *(result.requestedPreImage));
                }

                // It is legal for a delete operation on the pre-images collection to delete zero
                // documents - pre-image collections are not guaranteed to contain the same set of
                // documents at all times.
                if (result.nDeleted == 0 && mode == OplogApplication::Mode::kSecondary &&
                    !requestNss.isChangeStreamPreImagesCollection()) {
                    LOGV2_WARNING(2170002,
                                  "Applied a delete which did not delete anything in steady state "
                                  "replication",
                                  "op"_attr = redact(op.toBSONForLogging()));

                    // In FCV 4.4, each node is responsible for deleting the excess documents in
                    // capped collections. This implies that capped deletes may not be synchronized
                    // between nodes at times. When upgraded to FCV 5.0, the primary will generate
                    // delete oplog entries for capped collections. However, if any secondary was
                    // behind in deleting excess documents while in FCV 4.4, the primary would have
                    // no way of knowing and it would delete the first document it sees locally.
                    // Eventually, when secondaries step up and start deleting capped documents,
                    // they will first delete previously missed documents that may already be
                    // deleted on other nodes. For this reason we skip returning NoSuchKey for
                    // capped collections when oplog application is enforcing steady state
                    // constraints.
                    bool isCapped = false;
                    if (collection) {
                        isCapped = collection->isCapped();
                        opCounters->gotDeleteWasEmpty();
                    } else {
                        opCounters->gotDeleteFromMissingNamespace();
                    }

                    if (!isCapped) {
                        // This error is fatal when we are enforcing steady state constraints for
                        // non-capped collections.
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
        default: {
            // Commands are processed in applyCommand_inlock().
            invariant(false, str::stream() << "Unsupported opType " << OpType_serializer(opType));
        }
    }

    return Status::OK();
}

Status applyCommand_inlock(OperationContext* opCtx,
                           const OplogEntry& entry,
                           OplogApplication::Mode mode) {
    LOGV2_DEBUG(21255,
                3,
                "applying command op: {oplogEntry}, oplog application mode: "
                "{oplogApplicationMode}",
                "Applying command op",
                "oplogEntry"_attr = redact(entry.toBSONForLogging()),
                "oplogApplicationMode"_attr = OplogApplication::modeToString(mode));

    // Only commands are processed here.
    invariant(entry.getOpType() == OpTypeEnum::kCommand);

    // Choose opCounters based on running on standalone/primary or secondary by checking
    // whether writes are replicated.
    OpCounters* opCounters = opCtx->writesAreReplicated() ? &globalOpCounters : &replOpCounters;
    opCounters->gotCommand();

    BSONObj o = entry.getObject();

    const auto& nss = entry.getNss();
    if (!nss.isValid()) {
        return {ErrorCodes::InvalidNamespace, "invalid ns: " + std::string(nss.ns())};
    }
    {
        auto catalog = CollectionCatalog::get(opCtx);
        if (!catalog->lookupCollectionByNamespace(opCtx, nss) && catalog->lookupView(opCtx, nss)) {
            return {ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "applyOps not supported on view:" << nss.ns()};
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
        extractNs(nss.db(), o) == NamespaceString::kServerConfigurationNamespace) {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      str::stream() << "Applying command to feature compatibility version "
                                       "collection not supported in initial sync: "
                                    << redact(entry.toBSONForLogging()));
    }

    // Parse optime from oplog entry unless we are applying this command in standalone or on a
    // primary (replicated writes enabled).
    OpTime opTime;
    if (!opCtx->writesAreReplicated()) {
        opTime = entry.getOpTime();
    }

    const bool assignCommandTimestamp = [&] {
        const auto replMode = ReplicationCoordinator::get(opCtx)->getReplicationMode();
        if (opCtx->writesAreReplicated()) {
            // We do not assign timestamps on replicated writes since they will get their oplog
            // timestamp once they are logged.
            return false;
        }

        // Don't assign commit timestamp for transaction commands.
        const StringData commandName(o.firstElementFieldName());
        if (entry.shouldPrepare() ||
            entry.getCommandType() == OplogEntry::CommandType::kCommitTransaction ||
            entry.getCommandType() == OplogEntry::CommandType::kAbortTransaction)
            return false;

        switch (replMode) {
            case ReplicationCoordinator::modeReplSet: {
                // The timestamps in the command oplog entries are always real timestamps from this
                // oplog and we should timestamp our writes with them.
                return true;
            }
            case ReplicationCoordinator::modeNone: {
                // Only assign timestamps on standalones during replication recovery when
                // started with 'recoverFromOplogAsStandalone'.
                return mode == OplogApplication::Mode::kRecovering;
            }
        }
        MONGO_UNREACHABLE;
    }();
    invariant(!assignCommandTimestamp || !opTime.isNull(),
              str::stream() << "Oplog entry did not have 'ts' field when expected: "
                            << redact(entry.toBSONForLogging()));

    const Timestamp writeTime = (assignCommandTimestamp ? opTime.getTimestamp() : Timestamp());

    bool done = false;
    while (!done) {
        auto op = kOpsMap.find(o.firstElementFieldName());
        if (op == kOpsMap.end()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid key '" << o.firstElementFieldName()
                                        << "' found in field 'o'");
        }

        const ApplyOpMetadata& curOpToApply = op->second;

        Status status = [&] {
            try {
                // If 'writeTime' is not null, any writes in this scope will be given 'writeTime' as
                // their timestamp at commit.
                TimestampBlock tsBlock(opCtx, writeTime);
                return curOpToApply.applyFunc(opCtx, entry, mode);
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        }();

        switch (status.code()) {
            case ErrorCodes::WriteConflict: {
                // Need to throw this up to a higher level where it will be caught and the
                // operation retried.
                throwWriteConflictException(str::stream()
                                            << "WriteConflict caught during oplog application."
                                            << " Original error: " << status.reason());
            }
            case ErrorCodes::BackgroundOperationInProgressForDatabase: {
                invariant(mode == OplogApplication::Mode::kInitialSync);
                abortIndexBuilds(opCtx,
                                 entry.getCommandType(),
                                 nss,
                                 "Aborting index builds during initial sync");
                LOGV2_DEBUG(4665900,
                            1,
                            "Conflicting DDL operation encountered during initial sync; "
                            "aborting index build and retrying",
                            "db"_attr = nss.db());
                break;
            }
            case ErrorCodes::BackgroundOperationInProgressForNamespace: {
                Command* cmd = CommandHelpers::findCommand(o.firstElement().fieldName());
                invariant(cmd);

                auto ns = cmd->parse(opCtx, OpMsgRequest::fromDBAndBody(nss.db(), o))->ns();

                // This error is only possible during initial sync mode.
                invariant(mode == OplogApplication::Mode::kInitialSync);
                abortIndexBuilds(
                    opCtx, entry.getCommandType(), ns, "Aborting index builds during initial sync");
                LOGV2_DEBUG(4665901,
                            1,
                            "Conflicting DDL operation encountered during initial sync; "
                            "aborting index build and retrying",
                            logAttrs(ns));

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
                     status.code() != ErrorCodes::IndexNotFound && op->first != "dropDatabase") ||
                    !curOpToApply.acceptableErrors.count(status.code())) {
                    LOGV2_ERROR(21262,
                                "Failed command {command} on {db} with status {error} during oplog "
                                "application",
                                "Failed command during oplog application",
                                "command"_attr = redact(o),
                                "db"_attr = nss.db(),
                                "error"_attr = status);
                    return status;
                }

                if (mode == OplogApplication::Mode::kSecondary &&
                    status.code() != ErrorCodes::IndexNotFound) {
                    LOGV2_WARNING(2170000,
                                  "Acceptable error during oplog application",
                                  "db"_attr = nss.db(),
                                  "error"_attr = status,
                                  "oplogEntry"_attr = redact(entry.toBSONForLogging()));
                    opCounters->gotAcceptableErrorInCommand();
                } else {
                    LOGV2_DEBUG(51776,
                                1,
                                "Acceptable error during oplog application",
                                "db"_attr = nss.db(),
                                "error"_attr = status,
                                "oplogEntry"_attr = redact(entry.toBSONForLogging()));
                }
                [[fallthrough]];
            }
            case ErrorCodes::OK:
                done = true;
                break;
        }
    }

    AuthorizationManager::get(opCtx->getServiceContext())->logOp(opCtx, "c", nss, o, nullptr);
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
    LocalOplogInfo::get(opCtx)->setCollection(autoColl.getCollection());
}

void establishOplogCollectionForLogging(OperationContext* opCtx, const CollectionPtr& oplog) {
    invariant(opCtx->lockState()->isW());
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
