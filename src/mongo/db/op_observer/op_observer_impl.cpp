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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/create_indexes_gen.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/batched_write_context.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
using repl::DurableOplogEntry;
using repl::MutableOplogEntry;
using ChangeStreamPreImageRecordingMode = repl::ReplOperation::ChangeStreamPreImageRecordingMode;

const auto destinedRecipientDecoration =
    OplogDeleteEntryArgs::declareDecoration<boost::optional<ShardId>>();

namespace {

MONGO_FAIL_POINT_DEFINE(failCollectionUpdates);
MONGO_FAIL_POINT_DEFINE(hangAndFailUnpreparedCommitAfterReservingOplogSlot);

constexpr auto kNumRecordsFieldName = "numRecords"_sd;
constexpr auto kMsgFieldName = "msg"_sd;
constexpr long long kInvalidNumRecords = -1LL;

Date_t getWallClockTimeForOpLog(OperationContext* opCtx) {
    auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
    return clockSource->now();
}

repl::OpTime logOperation(OperationContext* opCtx,
                          MutableOplogEntry* oplogEntry,
                          bool assignWallClockTime,
                          OperationLogger* operationLogger) {
    if (assignWallClockTime) {
        oplogEntry->setWallClockTime(getWallClockTimeForOpLog(opCtx));
    }
    auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
    auto opTime = operationLogger->logOp(opCtx, oplogEntry);
    times.push_back(opTime);
    return opTime;
}

/**
 * Generic function that logs an operation.
 * Intended to reduce branching at call-sites by accepting the least common denominator
 * type: a MutableOplogEntry.
 *
 * 'fromMigrate' is generally hard-coded to false, but is supplied by a few
 * scenarios from mongos related behavior.
 *
 * If in a transaction, returns a null OpTime. Otherwise, returns the OpTime the operation
 * was logged with.
 */
repl::OpTime logMutableOplogEntry(OperationContext* opCtx,
                                  MutableOplogEntry* entry,
                                  OperationLogger* operationLogger,
                                  bool isRequiredInMultiDocumentTransaction = false) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    if (isRequiredInMultiDocumentTransaction) {
        invariant(inMultiDocumentTransaction);
    }

    if (inMultiDocumentTransaction) {
        txnParticipant.addTransactionOperation(opCtx, entry->toReplOperation());
        return {};
    } else {
        return logOperation(opCtx, entry, /*assignWallClockTime=*/true, operationLogger);
    }
}

/**
 * Updates the session state with the last write timestamp and transaction for that session.
 *
 * In the case of writes with transaction/statement id, this method will be recursively entered a
 * second time for the actual write to the transactions table. Since this write does not generate an
 * oplog entry, the recursion will stop at this point.
 */
void onWriteOpCompleted(OperationContext* opCtx,
                        std::vector<StmtId> stmtIdsWritten,
                        SessionTxnRecord sessionTxnRecord,
                        const NamespaceString& nss) {
    if (sessionTxnRecord.getLastWriteOpTime().isNull())
        return;

    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (!txnParticipant ||
        (!stmtIdsWritten.empty() && stmtIdsWritten.front() == kUninitializedStmtId))
        // If the first statement written in uninitialized, then all the statements are assumed to
        // be uninitialized.
        return;

    // We add these here since they may not exist if we return early.
    const auto lsid = *opCtx->getLogicalSessionId();
    sessionTxnRecord.setSessionId(lsid);
    if (isInternalSessionForRetryableWrite(lsid)) {
        sessionTxnRecord.setParentSessionId(*getParentSessionId(lsid));
    }
    sessionTxnRecord.setTxnNum(*opCtx->getTxnNumber());
    txnParticipant.onWriteOpCompletedOnPrimary(opCtx, std::move(stmtIdsWritten), sessionTxnRecord);

    if (!nss.isEmpty()) {
        txnParticipant.addToAffectedNamespaces(opCtx, nss);
    }
}

/**
 * Given the collection count from Collection::numRecords(), create and return the object for the
 * 'o2' field of a drop or rename oplog entry. If the collection count exceeds the upper limit of a
 * BSON NumberLong (long long), we will add a count of -1 and append a message with the original
 * collection count.
 *
 * Replication rollback uses this field to correct correction counts on drop-pending collections.
 */
BSONObj makeObject2ForDropOrRename(uint64_t numRecords) {
    BSONObjBuilder obj2Builder;
    if (numRecords > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
        obj2Builder.appendNumber(kNumRecordsFieldName, kInvalidNumRecords);
        std::string msg = str::stream() << "Collection count " << numRecords
                                        << " is larger than the "
                                           "maximum int64_t value. Setting numRecords to -1.";
        obj2Builder.append(kMsgFieldName, msg);
    } else {
        obj2Builder.appendNumber(kNumRecordsFieldName, static_cast<long long>(numRecords));
    }
    auto obj = obj2Builder.obj();
    return obj;
}

/**
 * Write oplog entry(ies) for the update operation.
 */
OpTimeBundle replLogUpdate(OperationContext* opCtx,
                           const OplogUpdateEntryArgs& args,
                           MutableOplogEntry* oplogEntry,
                           OperationLogger* operationLogger) {
    oplogEntry->setTid(args.coll->ns().tenantId());
    oplogEntry->setNss(args.coll->ns());
    oplogEntry->setUuid(args.coll->uuid());

    repl::OplogLink oplogLink;
    operationLogger->appendOplogEntryChainInfo(
        opCtx, oplogEntry, &oplogLink, args.updateArgs->stmtIds);

    OpTimeBundle opTimes;
    oplogEntry->setOpType(repl::OpTypeEnum::kUpdate);
    oplogEntry->setObject(args.updateArgs->update);
    oplogEntry->setObject2(args.updateArgs->criteria);
    oplogEntry->setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
    if (args.updateArgs->mustCheckExistenceForInsertOperations) {
        oplogEntry->setCheckExistenceForDiffInsert();
    }
    if (!args.updateArgs->oplogSlots.empty()) {
        oplogEntry->setOpTime(args.updateArgs->oplogSlots.back());
    }
    opTimes.writeOpTime =
        logOperation(opCtx, oplogEntry, true /*assignWallClockTime*/, operationLogger);
    opTimes.wallClockTime = oplogEntry->getWallClockTime();
    return opTimes;
}

/**
 * Write oplog entry(ies) for the delete operation.
 */
OpTimeBundle replLogDelete(OperationContext* opCtx,
                           const NamespaceString& nss,
                           MutableOplogEntry* oplogEntry,
                           const boost::optional<UUID>& uuid,
                           StmtId stmtId,
                           bool fromMigrate,
                           const DocumentKey& documentKey,
                           const boost::optional<ShardId>& destinedRecipient,
                           OperationLogger* operationLogger) {
    oplogEntry->setTid(nss.tenantId());
    oplogEntry->setNss(nss);
    oplogEntry->setUuid(uuid);
    oplogEntry->setDestinedRecipient(destinedRecipient);

    repl::OplogLink oplogLink;
    operationLogger->appendOplogEntryChainInfo(opCtx, oplogEntry, &oplogLink, {stmtId});

    OpTimeBundle opTimes;
    oplogEntry->setOpType(repl::OpTypeEnum::kDelete);
    oplogEntry->setObject(documentKey.getShardKeyAndId());
    oplogEntry->setFromMigrateIfTrue(fromMigrate);
    opTimes.writeOpTime =
        logOperation(opCtx, oplogEntry, true /*assignWallClockTime*/, operationLogger);
    opTimes.wallClockTime = oplogEntry->getWallClockTime();
    return opTimes;
}

bool shouldTimestampIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) {
    // This function returns whether a timestamp for a catalog write when beginning an index build,
    // or aborting an index build is necessary. There are four scenarios:

    // 1. A timestamp is already set -- replication application sets a timestamp ahead of time.
    // This could include the phase of initial sync where it applies oplog entries.  Also,
    // primaries performing an index build via `applyOps` may have a wrapping commit timestamp.
    if (!shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull())
        return false;

    // 2. If the node is initial syncing, we do not set a timestamp.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && replCoord->getMemberState().startup2())
        return false;

    // 3. If the index build is on the local database, do not timestamp.
    if (nss.isLocalDB())
        return false;

    // 4. All other cases, we generate a timestamp by writing a no-op oplog entry.  This is
    // better than using a ghost timestamp.  Writing an oplog entry ensures this node is
    // primary.
    return true;
}

void logGlobalIndexDDLOperation(OperationContext* opCtx,
                                const NamespaceString& globalIndexNss,
                                const UUID& globalIndexUUID,
                                const StringData commandString,
                                boost::optional<long long> numKeys,
                                OperationLogger* operationLogger) {
    invariant(!opCtx->inMultiDocumentTransaction());

    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, globalIndexNss)) {
        return;
    }

    BSONObjBuilder builder;
    // The rollback implementation requires the collection name to list affected namespaces.
    builder.append(commandString, globalIndexNss.coll());

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setObject(builder.done());

    // On global index drops, persist the number of records into the 'o2' field similar to a
    // collection drop. This allows for efficiently restoring the index keys count after rollback
    // without forcing a collection scan.
    invariant((numKeys && commandString == "dropGlobalIndex") ||
              (!numKeys && commandString == "createGlobalIndex"));
    if (numKeys) {
        oplogEntry.setObject2(makeObject2ForDropOrRename(*numKeys));
    }

    // The 'ns' field is technically redundant as it can be derived from the uuid, however it's a
    // required oplog entry field.
    oplogEntry.setNss(globalIndexNss.getCommandNS());
    oplogEntry.setUuid(globalIndexUUID);

    constexpr StmtId stmtId = 0;
    if (TransactionParticipant::get(opCtx)) {
        // This is a retryable write: populate the lsid, txnNumber and stmtId fields.
        // The oplog link to previous statement is empty and the stmtId is zero because this is a
        // single-statement command replicating as a single createGlobalIndex/dropGlobalIndex oplog
        // entry.
        repl::OplogLink oplogLink;
        operationLogger->appendOplogEntryChainInfo(opCtx, &oplogEntry, &oplogLink, {stmtId});
    }

    auto writeOpTime =
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, operationLogger);

    // Register the retryable write to in-memory transactions table.
    SessionTxnRecord sessionTxnRecord;
    sessionTxnRecord.setLastWriteOpTime(writeOpTime);
    sessionTxnRecord.setLastWriteDate(oplogEntry.getWallClockTime());
    onWriteOpCompleted(opCtx, {stmtId}, sessionTxnRecord, NamespaceString());
}

}  // namespace

OpObserverImpl::OpObserverImpl(std::unique_ptr<OperationLogger> operationLogger)
    : _operationLogger(std::move(operationLogger)) {}

void OpObserverImpl::onCreateGlobalIndex(OperationContext* opCtx,
                                         const NamespaceString& globalIndexNss,
                                         const UUID& globalIndexUUID) {
    constexpr StringData commandString = "createGlobalIndex"_sd;
    logGlobalIndexDDLOperation(opCtx,
                               globalIndexNss,
                               globalIndexUUID,
                               commandString,
                               boost::none /* numKeys */,
                               _operationLogger.get());
}

void OpObserverImpl::onDropGlobalIndex(OperationContext* opCtx,
                                       const NamespaceString& globalIndexNss,
                                       const UUID& globalIndexUUID,
                                       long long numKeys) {
    constexpr StringData commandString = "dropGlobalIndex"_sd;
    logGlobalIndexDDLOperation(
        opCtx, globalIndexNss, globalIndexUUID, commandString, numKeys, _operationLogger.get());
}

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }

    BSONObjBuilder builder;
    builder.append(CreateIndexesCommand::kCommandName, nss.coll());
    builder.appendElements(indexDoc);

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(builder.obj());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);

    auto opTime = logMutableOplogEntry(opCtx, &oplogEntry, _operationLogger.get());

    if (!serverGlobalParams.quiet.load()) {
        if (opTime.isNull()) {
            LOGV2(7360100,
                  "Added oplog entry for createIndexes to transaction",
                  logAttrs(oplogEntry.getNss()),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "object"_attr = oplogEntry.getObject());
        } else {
            LOGV2(7360101,
                  "Wrote oplog entry for createIndexes",
                  logAttrs(oplogEntry.getNss()),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "opTime"_attr = opTime,
                  "object"_attr = oplogEntry.getObject());
        }
    }
}

void OpObserverImpl::onStartIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       bool fromMigrate) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }

    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("startIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (const auto& indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
}

void OpObserverImpl::onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    if (!shouldTimestampIndexBuildSinglePhase(opCtx, nss)) {
        return;
    }

    onInternalOpMessage(
        opCtx,
        {},
        boost::none,
        BSON("msg" << std::string(str::stream() << "Creating indexes. Coll: "
                                                << NamespaceStringUtil::serialize(
                                                       nss, SerializationContext::stateDefault()))),
        boost::none,
        boost::none,
        boost::none,
        boost::none,
        boost::none);
}

void OpObserverImpl::onAbortIndexBuildSinglePhase(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    if (!shouldTimestampIndexBuildSinglePhase(opCtx, nss)) {
        return;
    }

    onInternalOpMessage(
        opCtx,
        {},
        boost::none,
        BSON("msg" << std::string(str::stream() << "Aborting indexes. Coll: "
                                                << NamespaceStringUtil::serialize(
                                                       nss, SerializationContext::stateDefault()))),
        boost::none,
        boost::none,
        boost::none,
        boost::none,
        boost::none);
}

void OpObserverImpl::onCommitIndexBuild(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const UUID& collUUID,
                                        const UUID& indexBuildUUID,
                                        const std::vector<BSONObj>& indexes,
                                        bool fromMigrate) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }

    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("commitIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (const auto& indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
}

void OpObserverImpl::onAbortIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       const Status& cause,
                                       bool fromMigrate) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }

    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("abortIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (const auto& indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    BSONObjBuilder causeBuilder(oplogEntryBuilder.subobjStart("cause"));
    // Some functions that extract a Status from a BSONObj, such as getStatusFromCommandResult(),
    // expect the 'ok' field.
    causeBuilder.appendBool("ok", 0);
    cause.serializeErrorToBSON(&causeBuilder);
    causeBuilder.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
}

namespace {

std::vector<repl::OpTime> _logInsertOps(OperationContext* opCtx,
                                        MutableOplogEntry* oplogEntryTemplate,
                                        std::vector<InsertStatement>::const_iterator begin,
                                        std::vector<InsertStatement>::const_iterator end,
                                        const std::vector<RecordId>& recordIds,
                                        const std::vector<bool>& fromMigrate,
                                        const ShardingWriteRouter& shardingWriteRouter,
                                        const CollectionPtr& collectionPtr,
                                        OperationLogger* operationLogger) {
    invariant(begin != end);

    auto nss = oplogEntryTemplate->getNss();
    // The number of entries in 'fromMigrate' should be consistent with the number of insert
    // operations in [begin, end). Also, 'fromMigrate' is a sharding concept, so there is no
    // need to check 'fromMigrate' for inserts that are not replicated. See SERVER-75829.
    invariant(std::distance(fromMigrate.begin(), fromMigrate.end()) == std::distance(begin, end),
              oplogEntryTemplate->toReplOperation().toBSON().toString());

    // If this oplog entry is from a tenant migration, include the tenant migration
    // UUID and optional donor timeline metadata.
    if (const auto& recipientInfo = repl::tenantMigrationInfo(opCtx)) {
        oplogEntryTemplate->setFromTenantMigration(recipientInfo->uuid);
        if (oplogEntryTemplate->getTid() &&
            change_stream_serverless_helpers::isChangeStreamEnabled(
                opCtx, *oplogEntryTemplate->getTid()) &&
            recipientInfo->donorOplogEntryData) {
            oplogEntryTemplate->setDonorOpTime(recipientInfo->donorOplogEntryData->donorOpTime);
            oplogEntryTemplate->setDonorApplyOpsIndex(
                recipientInfo->donorOplogEntryData->applyOpsIndex);
        }
    }

    const size_t count = end - begin;
    // Either no recordIds were passed in, or the number passed in is equal to the number
    // of inserts that happened.
    invariant(recordIds.empty() || recordIds.size() == count,
              str::stream() << "recordIds' size: " << recordIds.size()
                            << ", is non-empty but not equal to count: " << count);

    // Use OplogAccessMode::kLogOp to avoid recursive locking.
    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kLogOp);

    WriteUnitOfWork wuow(opCtx);

    std::vector<repl::OpTime> opTimes(count);
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
            insertStatementOplogSlot = operationLogger->getNextOpTimes(opCtx, 1U)[0];
        }
        const auto docKey = getDocumentKey(collectionPtr, begin[i].doc).getShardKeyAndId();
        if (!recordIds.empty()) {
            oplogEntry.setRecordId(recordIds[i]);
        }
        oplogEntry.setObject(begin[i].doc);
        oplogEntry.setObject2(docKey);
        oplogEntry.setOpTime(insertStatementOplogSlot);
        oplogEntry.setDestinedRecipient(
            shardingWriteRouter.getReshardingDestinedRecipient(begin[i].doc));
        addDestinedRecipient.execute([&](const BSONObj& data) {
            auto recipient = data["destinedRecipient"].String();
            oplogEntry.setDestinedRecipient(boost::make_optional<ShardId>({recipient}));
        });

        repl::OplogLink oplogLink;
        if (i > 0)
            oplogLink.prevOpTime = opTimes[i - 1];

        oplogEntry.setFromMigrateIfTrue(fromMigrate[i]);

        operationLogger->appendOplogEntryChainInfo(
            opCtx, &oplogEntry, &oplogLink, begin[i].stmtIds);

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
        LOGV2(7456300,
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
    auto wallClockTime = oplogEntryTemplate->getWallClockTime();
    operationLogger->logOplogRecords(opCtx,
                                     nss,
                                     &records,
                                     timestamps,
                                     oplogWrite.getCollection(),
                                     lastOpTime,
                                     wallClockTime,
                                     /*isAbortIndexBuild=*/false);
    wuow.commit();
    return opTimes;
}

bool _skipOplogOps(const bool isOplogDisabled,
                   const bool inBatchedWrite,
                   const bool inMultiDocumentTransaction,
                   const NamespaceString& nss,
                   const std::vector<StmtId>& stmtIds) {
    // Return early, possibly uassert for retryable writes if isOplogDisabledFor is true
    if (isOplogDisabled) {
        if (!inBatchedWrite && !inMultiDocumentTransaction) {
            invariant(!stmtIds.empty());
            invariant(stmtIds.front() != kUninitializedStmtId || stmtIds.size() == 1);
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "retryable writes is not supported for unreplicated ns: "
                                  << nss.toStringForErrorMsg(),
                    stmtIds.front() == kUninitializedStmtId);
        }
        return true;
    } else {
        return false;
    }
}

}  // namespace

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const CollectionPtr& coll,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               const std::vector<RecordId>& recordIds,
                               std::vector<bool> fromMigrate,
                               bool defaultFromMigrate,
                               OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isOplogDisabled = replCoord->isOplogDisabledFor(opCtx, nss);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && !isOplogDisabled && txnParticipant.transactionIsOpen();
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    if (_skipOplogOps(
            isOplogDisabled, inBatchedWrite, inMultiDocumentTransaction, nss, first->stmtIds)) {
        return;
    }

    const auto& uuid = coll->uuid();

    std::vector<repl::OpTime> opTimeList;
    repl::OpTime lastOpTime;

    auto shardingWriteRouter = std::make_unique<ShardingWriteRouter>(opCtx, nss);

    if (inBatchedWrite) {
        dassert(!defaultFromMigrate ||
                std::all_of(
                    fromMigrate.begin(), fromMigrate.end(), [](bool migrate) { return migrate; }));
        batchedWriteContext.setDefaultFromMigrate(defaultFromMigrate);

        size_t i = 0;
        for (auto iter = first; iter != last; iter++) {
            const auto docKey = getDocumentKey(coll, iter->doc).getShardKeyAndId();
            auto operation = MutableOplogEntry::makeInsertOperation(nss, uuid, iter->doc, docKey);
            operation.setDestinedRecipient(
                shardingWriteRouter->getReshardingDestinedRecipient(iter->doc));

            operation.setFromMigrateIfTrue(fromMigrate[std::distance(first, iter)]);
            if (!recordIds.empty()) {
                operation.setRecordId(recordIds[i++]);
            }
            operation.setInitializedStatementIds(iter->stmtIds);
            batchedWriteContext.addBatchedOperation(opCtx, operation);
        }
    } else if (inMultiDocumentTransaction) {
        invariant(!defaultFromMigrate);

        // Do not add writes to the profile collection to the list of transaction operations, since
        // these are done outside the transaction. There is no top-level WriteUnitOfWork when we are
        // in a SideTransactionBlock.
        if (!shard_role_details::getWriteUnitOfWork(opCtx)) {
            invariant(nss.isSystemDotProfile());
            return;
        }

        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());

        size_t i = 0;
        for (auto iter = first; iter != last; iter++) {
            const auto docKey = getDocumentKey(coll, iter->doc).getShardKeyAndId();
            auto operation = MutableOplogEntry::makeInsertOperation(nss, uuid, iter->doc, docKey);
            if (!recordIds.empty()) {
                operation.setRecordId(recordIds[i++]);
            }
            if (inRetryableInternalTransaction) {
                operation.setInitializedStatementIds(iter->stmtIds);
            }
            operation.setDestinedRecipient(
                shardingWriteRouter->getReshardingDestinedRecipient(iter->doc));

            operation.setFromMigrateIfTrue(fromMigrate[std::distance(first, iter)]);

            txnParticipant.addTransactionOperation(opCtx, operation);
        }
    } else {
        // Ensure well-formed embedded ReplOperation for logging.
        // This means setting optype, nss, and object at the minimum.
        MutableOplogEntry oplogEntryTemplate;
        oplogEntryTemplate.setOpType(repl::OpTypeEnum::kInsert);
        oplogEntryTemplate.setTid(nss.tenantId());
        oplogEntryTemplate.setNss(nss);
        oplogEntryTemplate.setUuid(uuid);
        oplogEntryTemplate.setObject({});
        oplogEntryTemplate.setFromMigrateIfTrue(defaultFromMigrate);
        Date_t lastWriteDate = getWallClockTimeForOpLog(opCtx);
        oplogEntryTemplate.setWallClockTime(lastWriteDate);

        opTimeList = _logInsertOps(opCtx,
                                   &oplogEntryTemplate,
                                   first,
                                   last,
                                   recordIds,
                                   std::move(fromMigrate),
                                   *shardingWriteRouter,
                                   coll,
                                   _operationLogger.get());
        if (!opTimeList.empty())
            lastOpTime = opTimeList.back();

        auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
        using std::begin;
        using std::end;
        times.insert(end(times), begin(opTimeList), end(opTimeList));

        std::vector<StmtId> stmtIdsWritten;
        std::for_each(first, last, [&](const InsertStatement& stmt) {
            stmtIdsWritten.insert(stmtIdsWritten.end(), stmt.stmtIds.begin(), stmt.stmtIds.end());
        });

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(lastOpTime);
        sessionTxnRecord.setLastWriteDate(lastWriteDate);
        onWriteOpCompleted(opCtx, stmtIdsWritten, sessionTxnRecord, nss);
    }

    if (opAccumulator) {
        opAccumulator->insertOpTimes = std::move(opTimeList);
        shardingWriteRouterOpStateAccumulatorDecoration(opAccumulator) =
            std::move(shardingWriteRouter);
    }
}

void OpObserverImpl::onInsertGlobalIndexKey(OperationContext* opCtx,
                                            const NamespaceString& globalIndexNss,
                                            const UUID& globalIndexUuid,
                                            const BSONObj& key,
                                            const BSONObj& docKey) {
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    invariant(!opCtx->isRetryableWrite());

    // _shardsvrInsertGlobalIndexKey must run inside a multi-doc transaction.
    bool isRequiredInMultiDocumentTransaction = true;

    MutableOplogEntry oplogEntry = MutableOplogEntry::makeGlobalIndexCrudOperation(
        repl::OpTypeEnum::kInsertGlobalIndexKey, globalIndexNss, globalIndexUuid, key, docKey);
    logMutableOplogEntry(
        opCtx, &oplogEntry, _operationLogger.get(), isRequiredInMultiDocumentTransaction);
}

void OpObserverImpl::onDeleteGlobalIndexKey(OperationContext* opCtx,
                                            const NamespaceString& globalIndexNss,
                                            const UUID& globalIndexUuid,
                                            const BSONObj& key,
                                            const BSONObj& docKey) {
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    invariant(!opCtx->isRetryableWrite());

    // _shardsvrDeleteGlobalIndexKey must run inside a multi-doc transaction.
    bool isRequiredInMultiDocumentTransaction = true;

    MutableOplogEntry oplogEntry = MutableOplogEntry::makeGlobalIndexCrudOperation(
        repl::OpTypeEnum::kDeleteGlobalIndexKey, globalIndexNss, globalIndexUuid, key, docKey);
    logMutableOplogEntry(
        opCtx, &oplogEntry, _operationLogger.get(), isRequiredInMultiDocumentTransaction);
}

void OpObserverImpl::onUpdate(OperationContext* opCtx,
                              const OplogUpdateEntryArgs& args,
                              OpStateAccumulator* opAccumulator) {
    failCollectionUpdates.executeIf(
        [&](const BSONObj&) {
            uasserted(40654,
                      str::stream() << "failCollectionUpdates failpoint enabled, namespace: "
                                    << args.coll->ns().toStringForErrorMsg()
                                    << ", update: " << args.updateArgs->update
                                    << " on document with " << args.updateArgs->criteria);
        },
        [&](const BSONObj& data) {
            // If the failpoint specifies no collection or matches the existing one, fail.
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "collectionNS");
            return fpNss.isEmpty() || args.coll->ns() == fpNss;
        });

    // Do not log a no-op operation; see SERVER-21738
    if (args.updateArgs->update.isEmpty()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const auto& nss = args.coll->ns();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isOplogDisabled = replCoord->isOplogDisabledFor(opCtx, nss);
    const bool inMultiDocumentTransaction =
        txnParticipant && !isOplogDisabled && txnParticipant.transactionIsOpen();
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    if (_skipOplogOps(isOplogDisabled,
                      inBatchedWrite,
                      inMultiDocumentTransaction,
                      nss,
                      args.updateArgs->stmtIds)) {
        return;
    }

    auto shardingWriteRouter = std::make_unique<ShardingWriteRouter>(opCtx, nss);
    OpTimeBundle opTime;
    if (inBatchedWrite) {
        auto operation = MutableOplogEntry::makeUpdateOperation(
            nss, args.coll->uuid(), args.updateArgs->update, args.updateArgs->criteria);
        operation.setDestinedRecipient(
            shardingWriteRouter->getReshardingDestinedRecipient(args.updateArgs->updatedDoc));
        operation.setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
        if (args.updateArgs->mustCheckExistenceForInsertOperations) {
            operation.setCheckExistenceForDiffInsert(true);
        }
        if (!args.updateArgs->replicatedRecordId.isNull()) {
            operation.setRecordId(args.updateArgs->replicatedRecordId);
        }
        batchedWriteContext.addBatchedOperation(opCtx, operation);
    } else if (inMultiDocumentTransaction) {
        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());

        invariant(
            inRetryableInternalTransaction ||
                args.retryableFindAndModifyLocation == RetryableFindAndModifyLocation::kNone,
            str::stream()
                << "Attempted a retryable write within a non-retryable multi-document transaction");

        auto operation = MutableOplogEntry::makeUpdateOperation(
            nss, args.coll->uuid(), args.updateArgs->update, args.updateArgs->criteria);

        if (inRetryableInternalTransaction) {
            operation.setInitializedStatementIds(args.updateArgs->stmtIds);
            if (args.updateArgs->storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
                invariant(!args.updateArgs->preImageDoc.isEmpty(),
                          str::stream()
                              << "Pre-image document must be present for pre-image recording");
                operation.setPreImage(args.updateArgs->preImageDoc.getOwned());
                operation.setPreImageRecordedForRetryableInternalTransaction();
                if (args.retryableFindAndModifyLocation ==
                    RetryableFindAndModifyLocation::kSideCollection) {
                    operation.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
                }
            }
            if (args.updateArgs->storeDocOption ==
                CollectionUpdateArgs::StoreDocOption::PostImage) {
                invariant(!args.updateArgs->updatedDoc.isEmpty(),
                          str::stream()
                              << "Update document must be present for post-image recording");
                operation.setPostImage(args.updateArgs->updatedDoc.getOwned());
                if (args.retryableFindAndModifyLocation ==
                    RetryableFindAndModifyLocation::kSideCollection) {
                    operation.setNeedsRetryImage(repl::RetryImageEnum::kPostImage);
                }
            }
        }

        if (!args.updateArgs->replicatedRecordId.isNull()) {
            operation.setRecordId(args.updateArgs->replicatedRecordId);
        }

        if (args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection) {
            invariant(!args.updateArgs->preImageDoc.isEmpty(),
                      str::stream()
                          << "Pre-image document must be present for pre-image recording");
            operation.setPreImage(args.updateArgs->preImageDoc.getOwned());
            operation.setChangeStreamPreImageRecordingMode(
                ChangeStreamPreImageRecordingMode::kPreImagesCollection);
        }

        const auto& scopedCollectionDescription = shardingWriteRouter->getCollDesc();
        // ShardingWriteRouter only has boost::none scopedCollectionDescription when not in a
        // sharded cluster.
        if (scopedCollectionDescription && scopedCollectionDescription->isSharded()) {
            operation.setPostImageDocumentKey(
                scopedCollectionDescription->extractDocumentKey(args.updateArgs->updatedDoc)
                    .getOwned());
        }

        operation.setDestinedRecipient(
            shardingWriteRouter->getReshardingDestinedRecipient(args.updateArgs->updatedDoc));
        operation.setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
        if (args.updateArgs->mustCheckExistenceForInsertOperations) {
            operation.setCheckExistenceForDiffInsert(true);
        }
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;
        oplogEntry.setDestinedRecipient(
            shardingWriteRouter->getReshardingDestinedRecipient(args.updateArgs->updatedDoc));

        if (args.retryableFindAndModifyLocation ==
            RetryableFindAndModifyLocation::kSideCollection) {
            // If we've stored a preImage:
            if (args.updateArgs->storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
                oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPreImage});
            } else if (args.updateArgs->storeDocOption ==
                       CollectionUpdateArgs::StoreDocOption::PostImage) {
                // Or if we're storing a postImage.
                oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPostImage});
            }
        }

        if (!args.updateArgs->replicatedRecordId.isNull()) {
            oplogEntry.setRecordId(args.updateArgs->replicatedRecordId);
        }

        opTime = replLogUpdate(opCtx, args, &oplogEntry, _operationLogger.get());
        if (opAccumulator) {
            opAccumulator->opTime.writeOpTime = opTime.writeOpTime;
            opAccumulator->opTime.wallClockTime = opTime.wallClockTime;

            // If the oplog entry has `needsRetryImage` (retryable findAndModify), gather the
            // pre/post image information to be stored in the the image collection.
            if (oplogEntry.getNeedsRetryImage()) {
                opAccumulator->retryableFindAndModifyImageToWrite =
                    repl::ReplOperation::ImageBundle{
                        oplogEntry.getNeedsRetryImage().value(),
                        /*imageDoc=*/
                        (oplogEntry.getNeedsRetryImage().value() == repl::RetryImageEnum::kPreImage
                             ? args.updateArgs->preImageDoc
                             : args.updateArgs->updatedDoc),
                        opTime.writeOpTime.getTimestamp()};
            }
        }

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(opTime.writeOpTime);
        sessionTxnRecord.setLastWriteDate(opTime.wallClockTime);
        onWriteOpCompleted(opCtx, args.updateArgs->stmtIds, sessionTxnRecord, nss);
    }

    if (opAccumulator) {
        shardingWriteRouterOpStateAccumulatorDecoration(opAccumulator) =
            std::move(shardingWriteRouter);
    }
}

void OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                   const CollectionPtr& coll,
                                   BSONObj const& doc,
                                   OplogDeleteEntryArgs* args,
                                   OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();
    documentKeyDecoration(args).emplace(getDocumentKey(coll, doc));

    // No need to create ShardingWriteRouter if isOplogDisabledFor is true.
    if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        ShardingWriteRouter shardingWriteRouter(opCtx, nss);
        destinedRecipientDecoration(args) = shardingWriteRouter.getReshardingDestinedRecipient(doc);
    }
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              StmtId stmtId,
                              const BSONObj& doc,
                              const OplogDeleteEntryArgs& args,
                              OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();
    const auto uuid = coll->uuid();
    auto optDocKey = documentKeyDecoration(args);
    invariant(optDocKey, nss.toStringForErrorMsg());
    auto& documentKey = optDocKey.value();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isOplogDisabled = replCoord->isOplogDisabledFor(opCtx, nss);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && !isOplogDisabled && txnParticipant.transactionIsOpen();
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    if (_skipOplogOps(isOplogDisabled, inBatchedWrite, inMultiDocumentTransaction, nss, {stmtId})) {
        return;
    }

    OpTimeBundle opTime;
    if (inBatchedWrite) {
        auto operation =
            MutableOplogEntry::makeDeleteOperation(nss, uuid, documentKey.getShardKeyAndId());
        operation.setDestinedRecipient(destinedRecipientDecoration(args));
        operation.setFromMigrateIfTrue(args.fromMigrate);

        if (!args.replicatedRecordId.isNull()) {
            operation.setRecordId(args.replicatedRecordId);
        }

        batchedWriteContext.addBatchedOperation(opCtx, operation);
    } else if (inMultiDocumentTransaction) {
        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());

        invariant(
            inRetryableInternalTransaction ||
                args.retryableFindAndModifyLocation == RetryableFindAndModifyLocation::kNone,
            str::stream()
                << "Attempted a retryable write within a non-retryable multi-document transaction");

        auto operation =
            MutableOplogEntry::makeDeleteOperation(nss, uuid, documentKey.getShardKeyAndId());

        if (!args.replicatedRecordId.isNull()) {
            operation.setRecordId(args.replicatedRecordId);
        }

        if (inRetryableInternalTransaction) {
            operation.setInitializedStatementIds({stmtId});
            if (args.retryableFindAndModifyLocation ==
                RetryableFindAndModifyLocation::kSideCollection) {
                invariant(!doc.isEmpty(),
                          str::stream()
                              << "Deleted document must be present for pre-image recording");
                operation.setPreImage(doc.getOwned());
                operation.setPreImageRecordedForRetryableInternalTransaction();
                operation.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
            }
        }

        if (args.changeStreamPreAndPostImagesEnabledForCollection) {
            invariant(!doc.isEmpty(),
                      str::stream() << "Deleted document must be present for pre-image recording");
            operation.setPreImage(doc.getOwned());
            operation.setChangeStreamPreImageRecordingMode(
                ChangeStreamPreImageRecordingMode::kPreImagesCollection);
        }

        operation.setDestinedRecipient(destinedRecipientDecoration(args));
        operation.setFromMigrateIfTrue(args.fromMigrate);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;

        if (args.retryableFindAndModifyLocation ==
            RetryableFindAndModifyLocation::kSideCollection) {
            invariant(!doc.isEmpty(),
                      str::stream() << "Deleted document must be present for pre-image recording");
            invariant(opCtx->getTxnNumber());

            oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPreImage});
            if (!args.retryableFindAndModifyOplogSlots.empty()) {
                oplogEntry.setOpTime(args.retryableFindAndModifyOplogSlots.back());
            }
        }

        if (!args.replicatedRecordId.isNull()) {
            oplogEntry.setRecordId(args.replicatedRecordId);
        }

        opTime = replLogDelete(opCtx,
                               nss,
                               &oplogEntry,
                               uuid,
                               stmtId,
                               args.fromMigrate,
                               documentKey,
                               destinedRecipientDecoration(args),
                               _operationLogger.get());
        if (opAccumulator) {
            opAccumulator->opTime.writeOpTime = opTime.writeOpTime;
            opAccumulator->opTime.wallClockTime = opTime.wallClockTime;

            // If the oplog entry has `needsRetryImage` (retryable findAndModify), gather the
            // pre/post image information to be stored in the the image collection.
            if (oplogEntry.getNeedsRetryImage()) {
                opAccumulator->retryableFindAndModifyImageToWrite =
                    repl::ReplOperation::ImageBundle{
                        repl::RetryImageEnum::kPreImage, doc, opTime.writeOpTime.getTimestamp()};
            }
        }

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(opTime.writeOpTime);
        sessionTxnRecord.setLastWriteDate(opTime.wallClockTime);
        onWriteOpCompleted(opCtx, std::vector<StmtId>{stmtId}, sessionTxnRecord, nss);
    }
}

void OpObserverImpl::onInternalOpMessage(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& uuid,
    const BSONObj& msgObj,
    const boost::optional<BSONObj> o2MsgObj,
    const boost::optional<repl::OpTime> preImageOpTime,
    const boost::optional<repl::OpTime> postImageOpTime,
    const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
    const boost::optional<OplogSlot> slot) {

    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(msgObj);
    oplogEntry.setObject2(o2MsgObj);
    oplogEntry.setPreImageOpTime(preImageOpTime);
    oplogEntry.setPostImageOpTime(postImageOpTime);
    oplogEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTimeInTransaction);
    if (slot) {
        oplogEntry.setOpTime(*slot);
    }
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex,
                                        const OplogSlot& createOpTime,
                                        bool fromMigrate) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setTid(collectionName.tenantId());
    oplogEntry.setNss(collectionName.getCommandNS());
    oplogEntry.setUuid(options.uuid);
    oplogEntry.setObject(MutableOplogEntry::makeCreateCollCmdObj(collectionName, options, idIndex));
    oplogEntry.setFromMigrateIfTrue(fromMigrate);

    if (!createOpTime.isNull()) {
        oplogEntry.setOpTime(createOpTime);
    }
    auto opTime = logMutableOplogEntry(opCtx, &oplogEntry, _operationLogger.get());
    if (!serverGlobalParams.quiet.load()) {
        if (opTime.isNull()) {
            LOGV2(7360102,
                  "Added oplog entry for create to transaction",
                  logAttrs(oplogEntry.getNss()),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "object"_attr = oplogEntry.getObject());
        } else {
            LOGV2(7360103,
                  "Wrote oplog entry for create",
                  logAttrs(oplogEntry.getNss()),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "opTime"_attr = opTime,
                  "object"_attr = oplogEntry.getObject());
        }
    }
}

void OpObserverImpl::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<IndexCollModInfo> indexInfo) {

    if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        // Create the 'o2' field object. We save the old collection metadata and TTL expiration.
        BSONObjBuilder o2Builder;
        o2Builder.append("collectionOptions_old", oldCollOptions.toBSON());
        if (indexInfo) {
            BSONObjBuilder oldIndexOptions;
            if (indexInfo->oldExpireAfterSeconds) {
                auto oldExpireAfterSeconds =
                    durationCount<Seconds>(indexInfo->oldExpireAfterSeconds.value());
                oldIndexOptions.append("expireAfterSeconds", oldExpireAfterSeconds);
            }
            if (indexInfo->oldHidden) {
                auto oldHidden = indexInfo->oldHidden.value();
                oldIndexOptions.append("hidden", oldHidden);
            }
            if (indexInfo->oldPrepareUnique) {
                auto oldPrepareUnique = indexInfo->oldPrepareUnique.value();
                oldIndexOptions.append("prepareUnique", oldPrepareUnique);
            }
            o2Builder.append("indexOptions_old", oldIndexOptions.obj());
        }

        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

        oplogEntry.setTid(nss.tenantId());
        oplogEntry.setNss(nss.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(makeCollModCmdObj(collModCmd, oldCollOptions, indexInfo));
        oplogEntry.setObject2(o2Builder.done());
        auto opTime =
            logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
        if (!serverGlobalParams.quiet.load()) {
            LOGV2(7360104,
                  "Wrote oplog entry for collMod",
                  logAttrs(oplogEntry.getNss()),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "opTime"_attr = opTime,
                  "object"_attr = oplogEntry.getObject());
        }
    }

    // Make sure the UUID values in the Collection metadata, the Collection object, and the UUID
    // catalog are all present and equal.
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X));
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.dbName());
    // Some unit tests call the op observer on an unregistered Database.
    if (!db) {
        return;
    }
    const Collection* coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);

    invariant(coll->uuid() == uuid);
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const DatabaseName& dbName) {
    const auto nss = NamespaceString::makeCommandNamespace(dbName);
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(dbName.tenantId());
    oplogEntry.setNss(nss);
    oplogEntry.setObject(BSON("dropDatabase" << 1));
    auto opTime =
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
    if (opCtx->writesAreReplicated() && !serverGlobalParams.quiet.load()) {
        LOGV2(7360105,
              "Wrote oplog entry for dropDatabase",
              logAttrs(oplogEntry.getNss()),
              "opTime"_attr = opTime,
              "object"_attr = oplogEntry.getObject());
    }

    uassert(50714, "dropping the admin database is not allowed.", !dbName.isAdminDB());
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType,
                                              bool markFromMigrate) {
    uassert(50715,
            "dropping the server configuration collection (admin.system.version) is not allowed.",
            collectionName != NamespaceString::kServerConfigurationNamespace);

    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
        return {};
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(collectionName.tenantId());
    oplogEntry.setNss(collectionName.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setFromMigrateIfTrue(markFromMigrate);
    oplogEntry.setObject(BSON("drop" << collectionName.coll()));
    oplogEntry.setObject2(makeObject2ForDropOrRename(numRecords));
    auto opTime =
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(7360106,
              "Wrote oplog entry for drop",
              logAttrs(oplogEntry.getNss()),
              "uuid"_attr = oplogEntry.getUuid(),
              "opTime"_attr = opTime,
              "object"_attr = oplogEntry.getObject());
    }

    return {};
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(BSON("dropIndexes" << nss.coll() << "index" << indexName));
    oplogEntry.setObject2(indexInfo);
    auto opTime =
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(7360107,
              "Wrote oplog entry for dropIndexes",
              logAttrs(oplogEntry.getNss()),
              "uuid"_attr = oplogEntry.getUuid(),
              "opTime"_attr = opTime,
              "object"_attr = oplogEntry.getObject());
    }
}

repl::OpTime OpObserverImpl::preRenameCollection(OperationContext* const opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 const UUID& uuid,
                                                 const boost::optional<UUID>& dropTargetUUID,
                                                 std::uint64_t numRecords,
                                                 bool stayTemp,
                                                 bool markFromMigrate) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, fromCollection)) {
        return {};
    }

    BSONObjBuilder builder;

    builder.append(
        "renameCollection",
        NamespaceStringUtil::serialize(fromCollection, SerializationContext::stateDefault()));
    builder.append(
        "to", NamespaceStringUtil::serialize(toCollection, SerializationContext::stateDefault()));
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(fromCollection.tenantId());
    oplogEntry.setNss(fromCollection.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setFromMigrateIfTrue(markFromMigrate);
    oplogEntry.setObject(builder.done());
    if (dropTargetUUID)
        oplogEntry.setObject2(makeObject2ForDropOrRename(numRecords));
    auto opTime =
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(7360108,
              "Wrote oplog entry for renameCollection",
              logAttrs(oplogEntry.getNss()),
              "uuid"_attr = oplogEntry.getUuid(),
              "opTime"_attr = opTime,
              "object"_attr = oplogEntry.getObject());
    }
    return {};
}

void OpObserverImpl::postRenameCollection(OperationContext* const opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          const UUID& uuid,
                                          const boost::optional<UUID>& dropTargetUUID,
                                          bool stayTemp) {
    if (fromCollection.isSystemDotViews())
        CollectionCatalog::get(opCtx)->reloadViews(opCtx, fromCollection.dbName());
    if (toCollection.isSystemDotViews())
        CollectionCatalog::get(opCtx)->reloadViews(opCtx, toCollection.dbName());
}

void OpObserverImpl::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        const UUID& uuid,
                                        const boost::optional<UUID>& dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp,
                                        bool markFromMigrate) {
    preRenameCollection(opCtx,
                        fromCollection,
                        toCollection,
                        uuid,
                        dropTargetUUID,
                        numRecords,
                        stayTemp,
                        markFromMigrate);
    postRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
}

void OpObserverImpl::onImportCollection(OperationContext* opCtx,
                                        const UUID& importUUID,
                                        const NamespaceString& nss,
                                        long long numRecords,
                                        long long dataSize,
                                        const BSONObj& catalogEntry,
                                        const BSONObj& storageMetadata,
                                        bool isDryRun) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }

    ImportCollectionOplogEntry importCollection(
        nss, importUUID, numRecords, dataSize, catalogEntry, storageMetadata, isDryRun);

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setObject(importCollection.toBSON());
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
}


void OpObserverImpl::onEmptyCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   const UUID& uuid) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(collectionName.tenantId());
    oplogEntry.setNss(collectionName.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(BSON("emptycapped" << collectionName.coll()));
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _operationLogger.get());
}

namespace {


/**
 * Returns maximum number of operations to pack into a single oplog entry,
 * when multi-oplog format for batched writes is in use.
 */
std::size_t getMaxNumberOfBatchedOperationsInSingleOplogEntry() {
    // IDL validation defined for this startup parameter ensures that we have a positive number.
    return static_cast<std::size_t>(gMaxNumberOfBatchedOperationsInSingleOplogEntry);
}

/**
 * Returns maximum size (bytes) of operations to pack into a single oplog entry,
 * when multi-oplog format for batched writes is in use.
 */
std::size_t getMaxSizeOfBatchedOperationsInSingleOplogEntryBytes() {
    // IDL validation defined for this startup parameter ensures that we have a positive number.
    return static_cast<std::size_t>(gMaxSizeOfBatchedOperationsInSingleOplogEntryBytes);
}

// Logs one applyOps entry on a prepared transaction, or an unprepared transaction's commit, or on
// committing a WUOW that is not necessarily tied to a multi-document transaction. It may update the
// transactions table on multi-document transactions. Assumes that the given BSON builder object
// already has  an 'applyOps' field appended pointing to the desired array of ops i.e. { "applyOps"
// : [op1, op2, ...] }
//
// @param txnState the 'state' field of the transaction table entry update.  @param startOpTime the
// optime of the 'startOpTime' field of the transaction table entry update. If boost::none, no
// 'startOpTime' field will be included in the new transaction table entry. Only meaningful if
// 'updateTxnTable' is true.  @param updateTxnTable determines whether the transactions table will
// updated after the oplog entry is written.
//
// Returns the optime of the written oplog entry.
repl::OpTime logApplyOps(OperationContext* opCtx,
                         MutableOplogEntry* oplogEntry,
                         boost::optional<DurableTxnStateEnum> txnState,
                         boost::optional<repl::OpTime> startOpTime,
                         std::vector<StmtId> stmtIdsWritten,
                         const bool updateTxnTable,
                         WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
                         OperationLogger* operationLogger) {

    const auto txnRetryCounter = opCtx->getTxnRetryCounter();
    if (oplogGroupingFormat == WriteUnitOfWork::kGroupForPossiblyRetryableOperations) {
        // If these operations have statement IDs, the applyOps is part of a retryable write so
        // we can use the normal oplog entry chain info call for it.
        if (!stmtIdsWritten.empty()) {
            repl::OplogLink oplogLink;
            oplogLink.prevOpTime =
                oplogEntry->getPrevWriteOpTimeInTransaction().value_or(repl::OpTime());
            oplogLink.multiOpType = repl::MultiOplogEntryType::kApplyOpsAppliedSeparately;
            operationLogger->appendOplogEntryChainInfo(
                opCtx, oplogEntry, &oplogLink, stmtIdsWritten);
        }
    } else {
        if (!stmtIdsWritten.empty()) {
            invariant(isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId()));
        }

        invariant(bool(txnRetryCounter) == bool(TransactionParticipant::get(opCtx)));

        // Batched writes (that is, WUOWs with 'oplogGroupingFormat ==
        // WriteUnitOfWork::kGroupForTransaction') are not associated with a txnNumber, so do not
        // emit an lsid either.
        oplogEntry->setSessionId(opCtx->getTxnNumber() ? opCtx->getLogicalSessionId()
                                                       : boost::none);
        oplogEntry->setTxnNumber(opCtx->getTxnNumber());
        if (txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
            oplogEntry->getOperationSessionInfo().setTxnRetryCounter(*txnRetryCounter);
        }
    }

    try {
        auto writeOpTime =
            logOperation(opCtx, oplogEntry, false /*assignWallClockTime*/, operationLogger);
        if (updateTxnTable) {
            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setLastWriteOpTime(writeOpTime);
            sessionTxnRecord.setLastWriteDate(oplogEntry->getWallClockTime());
            sessionTxnRecord.setState(txnState);
            sessionTxnRecord.setStartOpTime(startOpTime);
            if (txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
                sessionTxnRecord.setTxnRetryCounter(*txnRetryCounter);
            }
            onWriteOpCompleted(
                opCtx, std::move(stmtIdsWritten), sessionTxnRecord, NamespaceString());
        }
        return writeOpTime;
    } catch (const AssertionException& e) {
        // Change the error code to TransactionTooLarge if it is BSONObjectTooLarge.
        uassert(ErrorCodes::TransactionTooLarge,
                e.reason(),
                e.code() != ErrorCodes::BSONObjectTooLarge);
        throw;
    }
    MONGO_UNREACHABLE;
}

void logCommitOrAbortForPreparedTransaction(OperationContext* opCtx,
                                            MutableOplogEntry* oplogEntry,
                                            DurableTxnStateEnum durableState,
                                            OperationLogger* operationLogger) {
    const auto txnRetryCounter = *opCtx->getTxnRetryCounter();

    oplogEntry->setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry->setNss(NamespaceString::kAdminCommandNamespace);
    oplogEntry->setSessionId(opCtx->getLogicalSessionId());
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());
    if (!isDefaultTxnRetryCounter(txnRetryCounter)) {
        oplogEntry->getOperationSessionInfo().setTxnRetryCounter(txnRetryCounter);
    }
    oplogEntry->setPrevWriteOpTimeInTransaction(
        TransactionParticipant::get(opCtx).getLastWriteOpTime());

    // There should not be a parent WUOW outside of this one. This guarantees the safety of the
    // write conflict retry loop.
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // We must not have a maximum lock timeout, since writing the commit or abort oplog entry for a
    // prepared transaction must always succeed.
    invariant(!shard_role_details::getLocker(opCtx)->hasMaxLockTimeout());

    writeConflictRetry(
        opCtx, "onPreparedTransactionCommitOrAbort", NamespaceString::kRsOplogNamespace, [&] {
            // Writes to the oplog only require a Global intent lock. Guaranteed by
            // OplogSlotReserver.
            invariant(shard_role_details::getLocker(opCtx)->isWriteLocked());

            WriteUnitOfWork wuow(opCtx);
            const auto oplogOpTime =
                logOperation(opCtx, oplogEntry, true /*assignWallClockTime*/, operationLogger);
            invariant(oplogEntry->getOpTime().isNull() || oplogEntry->getOpTime() == oplogOpTime);

            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setLastWriteOpTime(oplogOpTime);
            sessionTxnRecord.setLastWriteDate(oplogEntry->getWallClockTime());
            sessionTxnRecord.setState(durableState);
            if (!isDefaultTxnRetryCounter(txnRetryCounter)) {
                sessionTxnRecord.setTxnRetryCounter(txnRetryCounter);
            }
            onWriteOpCompleted(opCtx, {}, sessionTxnRecord, NamespaceString());
            wuow.commit();
        });
}

}  // namespace

void OpObserverImpl::onTransactionStart(OperationContext* opCtx) {}

void OpObserverImpl::onUnpreparedTransactionCommit(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    OpStateAccumulator* opAccumulator) {
    const auto& statements = transactionOperations.getOperationsForOpObserver();

    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // It is possible that the transaction resulted in no changes.  In that case, we should
    // not write an empty applyOps entry.
    if (statements.empty())
        return;

    const auto& oplogSlots = reservedSlots;
    const auto& applyOpsOplogSlotAndOperationAssignment = applyOpsOperationAssignment;

    // Throw TenantMigrationConflict error if the database for the transaction statements is being
    // migrated. We only need check the namespace of the first statement since a transaction's
    // statements must all be for the same tenant.
    tenant_migration_access_blocker::checkIfCanWriteOrThrow(
        opCtx, statements.begin()->getNss().dbName(), oplogSlots.back().getTimestamp());

    if (MONGO_unlikely(hangAndFailUnpreparedCommitAfterReservingOplogSlot.shouldFail())) {
        hangAndFailUnpreparedCommitAfterReservingOplogSlot.pauseWhileSet(opCtx);
        uasserted(51268, "hangAndFailUnpreparedCommitAfterReservingOplogSlot fail point enabled");
    }

    invariant(!applyOpsOplogSlotAndOperationAssignment.prepare);
    const auto wallClockTime = getWallClockTimeForOpLog(opCtx);

    // Storage transaction commit is the last place inside a transaction that can throw an
    // exception. In order to safely allow exceptions to be thrown at that point, this function must
    // be called from an outer WriteUnitOfWork in order to be rolled back upon reaching the
    // exception.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // Writes to the oplog only require a Global intent lock. Guaranteed by
    // OplogSlotReserver.
    invariant(shard_role_details::getLocker(opCtx)->isWriteLocked());

    if (const auto& info = applyOpsOplogSlotAndOperationAssignment;
        info.applyOpsEntries.size() > 1U ||           // partial transaction
        info.numOperationsWithNeedsRetryImage > 0) {  // pre/post image to store in image collection
        // Partial transactions and unprepared transactions with pre or post image stored in the
        // image collection create/reserve multiple oplog entries in the same WriteUnitOfWork.
        // Because of this, such transactions will set multiple timestamps, violating the
        // multi timestamp constraint. It's safe to ignore the multi timestamp constraints here
        // as additional rollback logic is in place for this case. See SERVER-48771.
        shard_role_details::getRecoveryUnit(opCtx)->ignoreAllMultiTimestampConstraints();
    }

    auto logApplyOpsForUnpreparedTransaction =
        [opCtx, &oplogSlots, operationLogger = _operationLogger.get()](
            repl::MutableOplogEntry* oplogEntry,
            bool firstOp,
            bool lastOp,
            std::vector<StmtId> stmtIdsWritten,
            WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
            return logApplyOps(
                opCtx,
                oplogEntry,
                /*txnState=*/
                (lastOp ? DurableTxnStateEnum::kCommitted : DurableTxnStateEnum::kInProgress),
                /*startOpTime=*/boost::make_optional(!lastOp, oplogSlots.front()),
                std::move(stmtIdsWritten),
                /*updateTxnTable=*/(firstOp || lastOp),
                oplogGroupingFormat,
                operationLogger);
        };

    // Log in-progress entries for the transaction along with the implicit commit.
    boost::optional<repl::ReplOperation::ImageBundle> imageToWrite;
    auto numOplogEntries =
        transactionOperations.logOplogEntries(oplogSlots,
                                              applyOpsOplogSlotAndOperationAssignment,
                                              wallClockTime,
                                              WriteUnitOfWork::kDontGroup,
                                              logApplyOpsForUnpreparedTransaction,
                                              &imageToWrite);
    invariant(numOplogEntries > 0);

    repl::OpTime commitOpTime = oplogSlots[numOplogEntries - 1];
    invariant(!commitOpTime.isNull());
    if (opAccumulator) {
        opAccumulator->opTime.writeOpTime = commitOpTime;
        opAccumulator->opTime.wallClockTime = wallClockTime;
        opAccumulator->retryableFindAndModifyImageToWrite = imageToWrite;
    }
}

void OpObserverImpl::onBatchedWriteStart(OperationContext* opCtx) {
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    batchedWriteContext.setWritesAreBatched(true);
}

void OpObserverImpl::onBatchedWriteCommit(OperationContext* opCtx,
                                          WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
                                          OpStateAccumulator* opAccumulator) {
    if (!repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() ||
        !opCtx->writesAreReplicated()) {
        return;
    }

    // A batched write with oplogGroupingFormat kGroupForTransaction is a one-shot non-retryable
    // transaction without a transaction number, which is forbidden in retryable writes and
    // multi-document transactions.
    dassert(oplogGroupingFormat != WriteUnitOfWork::kGroupForTransaction || !opCtx->getTxnNumber());

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    // After the commit, make sure the batch is clear so we don't attempt to commit the same
    // operations twice.  The BatchedWriteContext is attached to the operation context, so multiple
    // sequential WriteUnitOfWork blocks can use the same batch context.
    ON_BLOCK_EXIT([&] {
        batchedWriteContext.clearBatchedOperations(opCtx);
        batchedWriteContext.setWritesAreBatched(false);
    });

    auto* batchedOps = batchedWriteContext.getBatchedOperations(opCtx);

    if (batchedOps->isEmpty()) {
        return;
    }

    // Reserve all the optimes in advance, so we only need to get the optime mutex once.  We
    // reserve enough entries for all statements in the transaction.
    auto oplogSlots = _operationLogger->getNextOpTimes(opCtx, batchedOps->numOperations());

    // Throw TenantMigrationConflict error if the database for the transaction statements is being
    // migrated. We only need check the namespace of the first statement since a transaction's
    // statements must all be for the same tenant.
    const auto& statements = batchedOps->getOperationsForOpObserver();
    const auto& firstOpNss = statements.begin()->getNss();
    tenant_migration_access_blocker::checkIfCanWriteOrThrow(
        opCtx, firstOpNss.dbName(), oplogSlots.back().getTimestamp());

    boost::optional<repl::ReplOperation::ImageBundle> noPrePostImage;

    // Serialize batched statements to BSON and determine their assignment to "applyOps"
    // entries.
    // By providing limits on operation count and size, this makes the processing of batched writes
    // more consistent with our treatment of multi-doc transactions.
    const auto applyOpsOplogSlotAndOperationAssignment =
        batchedOps->getApplyOpsInfo(oplogSlots,
                                    getMaxNumberOfBatchedOperationsInSingleOplogEntry(),
                                    getMaxSizeOfBatchedOperationsInSingleOplogEntryBytes(),
                                    /*prepare=*/false);

    if (!gFeatureFlagLargeBatchedOperations.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // Before SERVER-70765, we relied on packTransactionStatementsForApplyOps() to check if the
        // batch of operations could fit in a single applyOps entry. Now, we pass the size limit to
        // TransactionOperations::getApplyOpsInfo() and are now able to return an error earlier.
        // Previously, this used to be a tripwire assertion (tassert). This is now a uassert to be
        // consistent with packTransactionStatementsForApplyOps().
        uassert(ErrorCodes::TransactionTooLarge,
                "batched writes must generate a single applyOps entry",
                applyOpsOplogSlotAndOperationAssignment.applyOpsEntries.size() == 1);
    } else if (applyOpsOplogSlotAndOperationAssignment.applyOpsEntries.size() > 1) {
        // Batched writes spanning multiple oplog entries create/reserve multiple oplog entries in
        // the same WriteUnitOfWork. Because of this, such batched writes will set multiple
        // timestamps, violating the multi timestamp constraint. It's safe to ignore the multi
        // timestamp constraints here.
        shard_role_details::getRecoveryUnit(opCtx)->ignoreAllMultiTimestampConstraints();
    }

    // Storage transaction commit is the last place inside a transaction that can throw an
    // exception. In order to safely allow exceptions to be thrown at that point, this function must
    // be called from an outer WriteUnitOfWork in order to be rolled back upon reaching the
    // exception.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // Writes to the oplog only require a Global intent lock. Guaranteed by
    // OplogSlotReserver.
    invariant(shard_role_details::getLocker(opCtx)->isWriteLocked());

    // Batched writes do not violate the multiple timestamp constraint because they do not
    // replicate over multiple applyOps oplog entries or write pre/post images to the
    // image collection. However, multi-doc transactions may be replicated as a chain of
    // applyOps oplog entries in addition to potentially writing to the image collection.
    // Therefore, there are cases where the multiple timestamp constraint has to be relaxed
    // in order to replicate multi-doc transactions.
    // See onTransactionPrepare() and onUnpreparedTransactionCommit().
    invariant(applyOpsOplogSlotAndOperationAssignment.numOperationsWithNeedsRetryImage == 0,
              "batched writes must not contain pre/post images to store in image collection");

    auto logApplyOpsForBatchedWrite =
        [opCtx,
         defaultFromMigrate = batchedWriteContext.getDefaultFromMigrate(),
         operationLogger =
             _operationLogger.get()](repl::MutableOplogEntry* oplogEntry,
                                     bool firstOp,
                                     bool lastOp,
                                     std::vector<StmtId> stmtIdsWritten,
                                     WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
            // Remove 'prevOpTime' when replicating as a single applyOps oplog entry.
            // This preserves backwards compatibility with the legacy atomic applyOps oplog
            // entry format that we use to replicate batched writes.
            // OplogApplierImpl::_deriveOpsAndFillWriterVectors() enforces this restriction
            // using an invariant added in SERVER-43651.
            // For batched writes that replicate over a chain of applyOps oplog entries, we include
            // 'prevOpTime' so that oplog application is able to consume all the linked operations,
            // similar to large multi-document transactions. See SERVER-70572.
            if (firstOp && lastOp) {
                oplogEntry->setPrevWriteOpTimeInTransaction(boost::none);
            }
            oplogEntry->setFromMigrateIfTrue(defaultFromMigrate);
            const bool updateTxnTable =
                oplogGroupingFormat == WriteUnitOfWork::kGroupForPossiblyRetryableOperations;
            return logApplyOps(opCtx,
                               oplogEntry,
                               /*txnState=*/boost::none,
                               /*startOpTime=*/boost::none,
                               std::move(stmtIdsWritten),
                               updateTxnTable,
                               oplogGroupingFormat,
                               operationLogger);
        };

    const auto wallClockTime = getWallClockTimeForOpLog(opCtx);
    invariant(!applyOpsOplogSlotAndOperationAssignment.prepare);

    (void)batchedOps->logOplogEntries(oplogSlots,
                                      applyOpsOplogSlotAndOperationAssignment,
                                      wallClockTime,
                                      oplogGroupingFormat,
                                      logApplyOpsForBatchedWrite,
                                      &noPrePostImage);

    // Ensure the transactionParticipant properly tracks the namespaces affected by a
    // retryable batched write.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (txnParticipant) {
        for (const auto& statement : statements) {
            txnParticipant.addToAffectedNamespaces(opCtx, statement.getNss());
        }
    }

    if (opAccumulator) {
        for (const auto& entry : applyOpsOplogSlotAndOperationAssignment.applyOpsEntries) {
            opAccumulator->insertOpTimes.emplace_back(entry.oplogSlot);
        }
    }
}

void OpObserverImpl::onBatchedWriteAbort(OperationContext* opCtx) {
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    batchedWriteContext.clearBatchedOperations(opCtx);
    batchedWriteContext.setWritesAreBatched(false);
}

void OpObserverImpl::onPreparedTransactionCommit(
    OperationContext* opCtx,
    OplogSlot commitOplogEntryOpTime,
    Timestamp commitTimestamp,
    const std::vector<repl::ReplOperation>& statements) noexcept {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    invariant(!commitTimestamp.isNull());

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpTime(commitOplogEntryOpTime);

    CommitTransactionOplogObject cmdObj;
    cmdObj.setCommitTimestamp(commitTimestamp);
    oplogEntry.setObject(cmdObj.toBSON());

    logCommitOrAbortForPreparedTransaction(
        opCtx, &oplogEntry, DurableTxnStateEnum::kCommitted, _operationLogger.get());
}

void OpObserverImpl::onTransactionPrepare(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    size_t numberOfPrePostImagesToWrite,
    Date_t wallClockTime,
    OpStateAccumulator* opAccumulator) {
    invariant(!reservedSlots.empty());
    const auto prepareOpTime = reservedSlots.back();
    invariant(opCtx->getTxnNumber());
    invariant(!prepareOpTime.isNull());

    const auto& statements = transactionOperations.getOperationsForOpObserver();

    // Don't write oplog entry on secondaries.
    invariant(opCtx->writesAreReplicated());

    // We should have reserved enough slots.
    invariant(reservedSlots.size() >= statements.size());

    // Writes to the oplog only require a Global intent lock. Guaranteed by
    // OplogSlotReserver.
    invariant(shard_role_details::getLocker(opCtx)->isWriteLocked());

    // It is possible that the transaction resulted in no changes, In that case, we
    // should not write any operations other than the prepare oplog entry.
    if (!statements.empty()) {
        // Storage transaction commit is the last place inside a transaction that can
        // throw an exception. In order to safely allow exceptions to be thrown at that
        // point, this function must be called from an outer WriteUnitOfWork in order to
        // be rolled back upon reaching the exception.
        invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

        // Writes to the oplog only require a Global intent lock. Guaranteed by
        // OplogSlotReserver.
        invariant(shard_role_details::getLocker(opCtx)->isWriteLocked());

        if (applyOpsOperationAssignment.applyOpsEntries.size() > 1U) {
            // Partial transactions create/reserve multiple oplog entries in the same
            // WriteUnitOfWork. Because of this, such transactions will set multiple
            // timestamps, violating the multi timestamp constraint. It's safe to ignore
            // the multi timestamp constraints here as additional rollback logic is in
            // place for this case. See SERVER-48771.
            shard_role_details::getRecoveryUnit(opCtx)->ignoreAllMultiTimestampConstraints();
        }

        // This is set for every oplog entry, except for the last one, in the applyOps
        // chain of an unprepared multi-doc transaction.
        // For a single prepare oplog entry, choose the last oplog slot for the first
        // optime of the transaction. The first optime corresponds to the 'startOpTime'
        // field in SessionTxnRecord that is persisted in config.transactions.
        // See SERVER-40678.
        auto startOpTime = applyOpsOperationAssignment.applyOpsEntries.size() == 1U
            ? reservedSlots.back()
            : reservedSlots.front();

        auto logApplyOpsForPreparedTransaction =
            [opCtx, operationLogger = _operationLogger.get(), startOpTime](
                repl::MutableOplogEntry* oplogEntry,
                bool firstOp,
                bool lastOp,
                std::vector<StmtId> stmtIdsWritten,
                WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
                return logApplyOps(
                    opCtx,
                    oplogEntry,
                    /*txnState=*/
                    (lastOp ? DurableTxnStateEnum::kPrepared : DurableTxnStateEnum::kInProgress),
                    startOpTime,
                    std::move(stmtIdsWritten),
                    /*updateTxnTable=*/(firstOp || lastOp),
                    oplogGroupingFormat,
                    operationLogger);
            };

        // We had reserved enough oplog slots for the worst case where each operation
        // produced one oplog entry.  When operations are smaller and can be packed, we
        // will waste the extra slots.  The implicit prepare oplog entry will still use
        // the last reserved slot, because the transaction participant has already used
        // that as the prepare time.
        boost::optional<repl::ReplOperation::ImageBundle> imageToWrite;
        invariant(applyOpsOperationAssignment.prepare);
        (void)transactionOperations.logOplogEntries(reservedSlots,
                                                    applyOpsOperationAssignment,
                                                    wallClockTime,
                                                    WriteUnitOfWork::kDontGroup,
                                                    logApplyOpsForPreparedTransaction,
                                                    &imageToWrite);
        if (opAccumulator) {
            opAccumulator->retryableFindAndModifyImageToWrite = imageToWrite;
        }
    } else {
        // Log an empty 'prepare' oplog entry.
        // We need to have at least one reserved slot.
        invariant(reservedSlots.size() > 0);
        BSONObjBuilder applyOpsBuilder;
        BSONArrayBuilder opsArray(applyOpsBuilder.subarrayStart("applyOps"_sd));
        opsArray.done();
        applyOpsBuilder.append("prepare", true);

        auto oplogSlot = reservedSlots.front();
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(NamespaceString::kAdminCommandNamespace);
        oplogEntry.setOpTime(oplogSlot);
        oplogEntry.setPrevWriteOpTimeInTransaction(repl::OpTime());
        oplogEntry.setObject(applyOpsBuilder.done());
        oplogEntry.setWallClockTime(wallClockTime);

        // TODO SERVER-69286: set the top-level tenantId here

        logApplyOps(opCtx,
                    &oplogEntry,
                    DurableTxnStateEnum::kPrepared,
                    /*startOpTime=*/oplogSlot,
                    /*stmtIdsWritten=*/{},
                    /*updateTxnTable=*/true,
                    WriteUnitOfWork::kDontGroup,
                    _operationLogger.get());
    }
}

void OpObserverImpl::onTransactionPrepareNonPrimary(OperationContext* opCtx,
                                                    const LogicalSessionId& lsid,
                                                    const std::vector<repl::OplogEntry>& statements,
                                                    const repl::OpTime& prepareOpTime) {}

void OpObserverImpl::onTransactionAbort(OperationContext* opCtx,
                                        boost::optional<OplogSlot> abortOplogEntryOpTime) {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);

    if (!abortOplogEntryOpTime) {
        invariant(!txnParticipant.transactionIsCommitted());
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpTime(*abortOplogEntryOpTime);

    AbortTransactionOplogObject cmdObj;
    oplogEntry.setObject(cmdObj.toBSON());

    logCommitOrAbortForPreparedTransaction(
        opCtx, &oplogEntry, DurableTxnStateEnum::kAborted, _operationLogger.get());
}

void OpObserverImpl::onModifyCollectionShardingIndexCatalog(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            const UUID& uuid,
                                                            BSONObj opDoc) {
    repl::MutableOplogEntry oplogEntry;
    auto obj = BSON(kShardingIndexCatalogOplogEntryName
                    << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()))
                   .addFields(opDoc);
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(obj);

    logOperation(opCtx, &oplogEntry, true, _operationLogger.get());
}

void OpObserverImpl::onReplicationRollback(OperationContext* opCtx,
                                           const RollbackObserverInfo& rbInfo) {
    // Reset the key manager cache.
    auto validator = LogicalTimeValidator::get(opCtx);
    if (validator) {
        validator->resetKeyManagerCache();
    }

    // Check if the shard identity document rolled back.
    if (rbInfo.shardIdentityRolledBack) {
        fassertFailedNoTrace(50712);
    }

    // Force the default read/write concern cache to reload on next access in case the defaults
    // document was rolled back.
    ReadWriteConcernDefaults::get(opCtx).invalidate();
}

}  // namespace mongo
