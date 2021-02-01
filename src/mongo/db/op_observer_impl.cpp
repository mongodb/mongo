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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/op_observer_impl.h"

#include <limits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {
using repl::DurableOplogEntry;
using repl::MutableOplogEntry;
const OperationContext::Decoration<boost::optional<OpObserverImpl::DocumentKey>>
    documentKeyDecoration =
        OperationContext::declareDecoration<boost::optional<OpObserverImpl::DocumentKey>>();

const OperationContext::Decoration<boost::optional<ShardId>> destinedRecipientDecoration =
    OperationContext::declareDecoration<boost::optional<ShardId>>();

namespace {

MONGO_FAIL_POINT_DEFINE(failCollectionUpdates);
MONGO_FAIL_POINT_DEFINE(hangAndFailUnpreparedCommitAfterReservingOplogSlot);
MONGO_FAIL_POINT_DEFINE(hangAfterLoggingApplyOpsForTransaction);

constexpr auto kNumRecordsFieldName = "numRecords"_sd;
constexpr auto kMsgFieldName = "msg"_sd;
constexpr long long kInvalidNumRecords = -1LL;

Date_t getWallClockTimeForOpLog(OperationContext* opCtx) {
    auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
    return clockSource->now();
}

repl::OpTime logOperation(OperationContext* opCtx, MutableOplogEntry* oplogEntry) {
    oplogEntry->setWallClockTime(getWallClockTimeForOpLog(opCtx));
    auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
    auto opTime = repl::logOp(opCtx, oplogEntry);
    times.push_back(opTime);
    return opTime;
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
                        SessionTxnRecord sessionTxnRecord) {
    if (sessionTxnRecord.getLastWriteOpTime().isNull())
        return;

    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (!txnParticipant)
        return;

    // We add these here since they may not exist if we return early.
    sessionTxnRecord.setSessionId(*opCtx->getLogicalSessionId());
    sessionTxnRecord.setTxnNum(*opCtx->getTxnNumber());
    txnParticipant.onWriteOpCompletedOnPrimary(opCtx, std::move(stmtIdsWritten), sessionTxnRecord);
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

struct OpTimeBundle {
    repl::OpTime writeOpTime;
    repl::OpTime prePostImageOpTime;
    Date_t wallClockTime;
};

/**
 * Write oplog entry(ies) for the update operation.
 */
OpTimeBundle replLogUpdate(OperationContext* opCtx,
                           const OplogUpdateEntryArgs& args,
                           MutableOplogEntry oplogEntry) {
    oplogEntry.setNss(args.nss);
    oplogEntry.setUuid(args.uuid);

    repl::OplogLink oplogLink;
    repl::appendOplogEntryChainInfo(opCtx, &oplogEntry, &oplogLink, args.updateArgs.stmtId);

    OpTimeBundle opTimes;
    // We never want to store pre- or post- images when we're migrating oplog entries from another
    // replica set.
    const auto& migrationRecipientInfo = repl::tenantMigrationRecipientInfo(opCtx);
    const auto storePreImageForRetryableWrite =
        (args.updateArgs.storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage &&
         opCtx->getTxnNumber());
    if ((storePreImageForRetryableWrite || args.updateArgs.preImageRecordingEnabledForCollection) &&
        !migrationRecipientInfo) {
        MutableOplogEntry noopEntry = oplogEntry;
        invariant(args.updateArgs.preImageDoc);
        noopEntry.setOpType(repl::OpTypeEnum::kNoop);
        noopEntry.setObject(*args.updateArgs.preImageDoc);
        oplogLink.preImageOpTime = logOperation(opCtx, &noopEntry);
        if (storePreImageForRetryableWrite) {
            opTimes.prePostImageOpTime = oplogLink.preImageOpTime;
        }
    }

    // This case handles storing the post image for retryable findAndModify's.
    if (args.updateArgs.storeDocOption == CollectionUpdateArgs::StoreDocOption::PostImage &&
        opCtx->getTxnNumber() && !migrationRecipientInfo) {
        MutableOplogEntry noopEntry = oplogEntry;
        noopEntry.setOpType(repl::OpTypeEnum::kNoop);
        noopEntry.setObject(args.updateArgs.updatedDoc);
        oplogLink.postImageOpTime = logOperation(opCtx, &noopEntry);
        invariant(opTimes.prePostImageOpTime.isNull());
        opTimes.prePostImageOpTime = oplogLink.postImageOpTime;
    }

    oplogEntry.setOpType(repl::OpTypeEnum::kUpdate);
    oplogEntry.setObject(args.updateArgs.update);
    oplogEntry.setObject2(args.updateArgs.criteria);
    oplogEntry.setFromMigrateIfTrue(args.updateArgs.fromMigrate);
    // oplogLink could have been changed to include pre/postImageOpTime by the previous no-op write.
    repl::appendOplogEntryChainInfo(opCtx, &oplogEntry, &oplogLink, args.updateArgs.stmtId);
    if (args.updateArgs.oplogSlot) {
        oplogEntry.setOpTime(*args.updateArgs.oplogSlot);
    }
    opTimes.writeOpTime = logOperation(opCtx, &oplogEntry);
    opTimes.wallClockTime = oplogEntry.getWallClockTime();
    return opTimes;
}

/**
 * Write oplog entry(ies) for the delete operation.
 */
OpTimeBundle replLogDelete(OperationContext* opCtx,
                           const NamespaceString& nss,
                           OptionalCollectionUUID uuid,
                           StmtId stmtId,
                           bool fromMigrate,
                           const boost::optional<BSONObj>& deletedDoc) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setDestinedRecipient(destinedRecipientDecoration(opCtx));

    repl::OplogLink oplogLink;
    repl::appendOplogEntryChainInfo(opCtx, &oplogEntry, &oplogLink, stmtId);

    OpTimeBundle opTimes;
    // We never want to store pre-images when we're migrating oplog entries from another
    // replica set.
    const auto& migrationRecipientInfo = repl::tenantMigrationRecipientInfo(opCtx);
    if (deletedDoc && !migrationRecipientInfo) {
        MutableOplogEntry noopEntry = oplogEntry;
        noopEntry.setOpType(repl::OpTypeEnum::kNoop);
        noopEntry.setObject(*deletedDoc);
        auto noteOplog = logOperation(opCtx, &noopEntry);
        opTimes.prePostImageOpTime = noteOplog;
        oplogLink.preImageOpTime = noteOplog;
    }

    oplogEntry.setOpType(repl::OpTypeEnum::kDelete);
    oplogEntry.setObject(documentKeyDecoration(opCtx).get().getShardKeyAndId());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    // oplogLink could have been changed to include preImageOpTime by the previous no-op write.
    repl::appendOplogEntryChainInfo(opCtx, &oplogEntry, &oplogLink, stmtId);
    opTimes.writeOpTime = logOperation(opCtx, &oplogEntry);
    opTimes.wallClockTime = oplogEntry.getWallClockTime();
    return opTimes;
}

}  // namespace

BSONObj OpObserverImpl::DocumentKey::getId() const {
    return _id;
}

BSONObj OpObserverImpl::DocumentKey::getShardKeyAndId() const {
    if (_shardKey) {
        BSONObjBuilder builder(_shardKey.get());
        builder.appendElementsUnique(_id);
        return builder.obj();
    }

    // _shardKey is not set so just return the _id.
    return getId();
}

OpObserverImpl::DocumentKey OpObserverImpl::getDocumentKey(OperationContext* opCtx,
                                                           NamespaceString const& nss,
                                                           BSONObj const& doc) {
    BSONObj id = doc["_id"] ? doc["_id"].wrap() : doc;
    boost::optional<BSONObj> shardKey;

    // Extract the shard key from the collection description in the CollectionShardingState
    // if running on standalone or primary. Skip this completely on secondaries since they are
    // not expected to have the collection metadata cached.
    if (opCtx->writesAreReplicated()) {
        auto collDesc = CollectionShardingState::get(opCtx, nss)->getCollectionDescription(opCtx);
        if (collDesc.isSharded()) {
            shardKey =
                dotted_path_support::extractElementsBasedOnTemplate(doc, collDesc.getKeyPattern())
                    .getOwned();
        }
    }

    return {std::move(id), std::move(shardKey)};
}

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   CollectionUUID uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    if (inMultiDocumentTransaction) {
        auto operation = MutableOplogEntry::makeCreateIndexesCommand(nss, uuid, indexDoc);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        BSONObjBuilder builder;
        builder.append("createIndexes", nss.coll());
        builder.appendElements(indexDoc);

        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(nss.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(builder.done());
        oplogEntry.setFromMigrateIfTrue(fromMigrate);
        logOperation(opCtx, &oplogEntry);
    }
}

void OpObserverImpl::onStartIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       CollectionUUID collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("startIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (auto indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    // This function sets a timestamp for the initial catalog write when beginning an index
    // build, if necessary.  There are four scenarios:

    // 1. A timestamp is already set -- replication application sets a timestamp ahead of time.
    // This could include the phase of initial sync where it applies oplog entries.  Also,
    // primaries performing an index build via `applyOps` may have a wrapping commit timestamp.
    if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull())
        return;

    // 2. If the node is initial syncing, we do not set a timestamp.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->isReplEnabled() && replCoord->getMemberState().startup2())
        return;

    // 3. If the index build is on the local database, do not timestamp.
    if (nss.isLocal())
        return;

    // 4. All other cases, we generate a timestamp by writing a no-op oplog entry.  This is
    // better than using a ghost timestamp.  Writing an oplog entry ensures this node is
    // primary.
    onInternalOpMessage(
        opCtx,
        {},
        boost::none,
        BSON("msg" << std::string(str::stream() << "Creating indexes. Coll: " << nss)),
        boost::none,
        boost::none,
        boost::none,
        boost::none,
        boost::none);
}

void OpObserverImpl::onCommitIndexBuild(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        CollectionUUID collUUID,
                                        const UUID& indexBuildUUID,
                                        const std::vector<BSONObj>& indexes,
                                        bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("commitIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (auto indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onAbortIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       CollectionUUID collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       const Status& cause,
                                       bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("abortIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (auto indexDoc : indexes) {
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
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               bool fromMigrate) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    Date_t lastWriteDate;

    std::vector<repl::OpTime> opTimeList;
    repl::OpTime lastOpTime;

    if (inMultiDocumentTransaction) {
        // Do not add writes to the profile collection to the list of transaction operations, since
        // these are done outside the transaction. There is no top-level WriteUnitOfWork when we are
        // in a SideTransactionBlock.
        if (!opCtx->getWriteUnitOfWork()) {
            invariant(nss.isSystemDotProfile());
            return;
        }

        for (auto iter = first; iter != last; iter++) {
            auto operation = MutableOplogEntry::makeInsertOperation(nss, uuid.get(), iter->doc);
            shardAnnotateOplogEntry(opCtx, nss, iter->doc, operation);
            txnParticipant.addTransactionOperation(opCtx, operation);
        }
    } else {
        MutableOplogEntry oplogEntryTemplate;
        oplogEntryTemplate.setNss(nss);
        oplogEntryTemplate.setUuid(uuid);
        oplogEntryTemplate.setFromMigrateIfTrue(fromMigrate);
        lastWriteDate = getWallClockTimeForOpLog(opCtx);
        oplogEntryTemplate.setWallClockTime(lastWriteDate);

        opTimeList = repl::logInsertOps(opCtx, &oplogEntryTemplate, first, last);
        if (!opTimeList.empty())
            lastOpTime = opTimeList.back();

        auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
        using std::begin;
        using std::end;
        times.insert(end(times), begin(opTimeList), end(opTimeList));

        std::vector<StmtId> stmtIdsWritten;
        std::transform(first,
                       last,
                       std::back_inserter(stmtIdsWritten),
                       [](const InsertStatement& stmt) { return stmt.stmtId; });

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(lastOpTime);
        sessionTxnRecord.setLastWriteDate(lastWriteDate);
        onWriteOpCompleted(opCtx, stmtIdsWritten, sessionTxnRecord);
    }

    size_t index = 0;
    for (auto it = first; it != last; it++, index++) {
        auto opTime = opTimeList.empty() ? repl::OpTime() : opTimeList[index];
        shardObserveInsertOp(opCtx, nss, it->doc, opTime, fromMigrate, inMultiDocumentTransaction);
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace && !lastOpTime.isNull()) {
        for (auto it = first; it != last; it++) {
            MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx, it->doc);
        }
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        for (auto it = first; it != last; it++) {
            ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
                opCtx, it->doc["_id"], it->doc);
        }
    }
}

void OpObserverImpl::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    failCollectionUpdates.executeIf(
        [&](const BSONObj&) {
            uasserted(40654,
                      str::stream() << "failCollectionUpdates failpoint enabled, namespace: "
                                    << args.nss.ns() << ", update: " << args.updateArgs.update
                                    << " on document with " << args.updateArgs.criteria);
        },
        [&](const BSONObj& data) {
            // If the failpoint specifies no collection or matches the existing one, fail.
            auto collElem = data["collectionNS"];
            return !collElem || args.nss.ns() == collElem.String();
        });

    // Do not log a no-op operation; see SERVER-21738
    if (args.updateArgs.update.isEmpty()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    OpTimeBundle opTime;
    if (inMultiDocumentTransaction) {
        auto operation = MutableOplogEntry::makeUpdateOperation(
            args.nss, args.uuid, args.updateArgs.update, args.updateArgs.criteria);

        shardAnnotateOplogEntry(opCtx, args.nss, args.updateArgs.updatedDoc, operation);

        if (args.updateArgs.preImageRecordingEnabledForCollection) {
            invariant(args.updateArgs.preImageDoc);
            operation.setPreImage(args.updateArgs.preImageDoc->getOwned());
        }

        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;
        shardAnnotateOplogEntry(
            opCtx, args.nss, args.updateArgs.updatedDoc, oplogEntry.getDurableReplOperation());
        opTime = replLogUpdate(opCtx, args, std::move(oplogEntry));

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(opTime.writeOpTime);
        sessionTxnRecord.setLastWriteDate(opTime.wallClockTime);
        onWriteOpCompleted(opCtx, std::vector<StmtId>{args.updateArgs.stmtId}, sessionTxnRecord);
    }

    if (args.nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!args.updateArgs.fromMigrate) {
            shardObserveUpdateOp(opCtx,
                                 args.nss,
                                 args.updateArgs.preImageDoc,
                                 args.updateArgs.updatedDoc,
                                 opTime.writeOpTime,
                                 opTime.prePostImageOpTime,
                                 inMultiDocumentTransaction);
        }
    }

    if (args.nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (args.nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, args.nss);
    } else if (args.nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx,
                                                                     args.updateArgs.updatedDoc);
    } else if (args.nss == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
            opCtx, args.updateArgs.updatedDoc["_id"], args.updateArgs.updatedDoc);
    }
}

void OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                   NamespaceString const& nss,
                                   BSONObj const& doc) {
    documentKeyDecoration(opCtx).emplace(getDocumentKey(opCtx, nss, doc));

    repl::DurableReplOperation op;
    shardAnnotateOplogEntry(opCtx, nss, doc, op);
    destinedRecipientDecoration(opCtx) = op.getDestinedRecipient();

    shardObserveAboutToDelete(opCtx, nss, doc);
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const NamespaceString& nss,
                              OptionalCollectionUUID uuid,
                              StmtId stmtId,
                              bool fromMigrate,
                              const boost::optional<BSONObj>& deletedDoc) {
    auto optDocKey = documentKeyDecoration(opCtx);
    invariant(optDocKey, nss.ns());
    auto& documentKey = optDocKey.get();

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    OpTimeBundle opTime;
    if (inMultiDocumentTransaction) {
        auto operation =
            MutableOplogEntry::makeDeleteOperation(nss, uuid.get(), documentKey.getShardKeyAndId());
        if (deletedDoc) {
            operation.setPreImage(deletedDoc->getOwned());
        }

        operation.setDestinedRecipient(destinedRecipientDecoration(opCtx));

        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        opTime = replLogDelete(opCtx, nss, uuid, stmtId, fromMigrate, deletedDoc);
        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(opTime.writeOpTime);
        sessionTxnRecord.setLastWriteDate(opTime.wallClockTime);
        onWriteOpCompleted(opCtx, std::vector<StmtId>{stmtId}, sessionTxnRecord);
    }

    if (nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!fromMigrate) {
            shardObserveDeleteOp(opCtx,
                                 nss,
                                 documentKey.getId(),
                                 opTime.writeOpTime,
                                 opTime.prePostImageOpTime,
                                 inMultiDocumentTransaction);
        }
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx, documentKey.getId());
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
            opCtx, documentKey.getId().firstElement(), boost::none);
    }
}

void OpObserverImpl::onInternalOpMessage(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID> uuid,
    const BSONObj& msgObj,
    const boost::optional<BSONObj> o2MsgObj,
    const boost::optional<repl::OpTime> preImageOpTime,
    const boost::optional<repl::OpTime> postImageOpTime,
    const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
    const boost::optional<OplogSlot> slot) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
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
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex,
                                        const OplogSlot& createOpTime) {
    // do not replicate system.profile modifications
    if (collectionName.isSystemDotProfile()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    if (inMultiDocumentTransaction) {
        auto operation = MutableOplogEntry::makeCreateCommand(collectionName, options, idIndex);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(collectionName.getCommandNS());
        oplogEntry.setUuid(options.uuid);
        oplogEntry.setObject(
            MutableOplogEntry::makeCreateCollCmdObj(collectionName, options, idIndex));
        oplogEntry.setOpTime(createOpTime);
        logOperation(opCtx, &oplogEntry);
    }
}

void OpObserverImpl::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<IndexCollModInfo> indexInfo) {

    if (!nss.isSystemDotProfile()) {
        // do not replicate system.profile modifications

        // Create the 'o2' field object. We save the old collection metadata and TTL expiration.
        BSONObjBuilder o2Builder;
        o2Builder.append("collectionOptions_old", oldCollOptions.toBSON());
        if (indexInfo) {
            if (indexInfo->oldExpireAfterSeconds) {
                auto oldExpireAfterSeconds =
                    durationCount<Seconds>(indexInfo->oldExpireAfterSeconds.get());
                o2Builder.append("expireAfterSeconds_old", oldExpireAfterSeconds);
            }
            if (indexInfo->oldHidden) {
                auto oldHidden = indexInfo->oldHidden.get();
                o2Builder.append("hidden_old", oldHidden);
            }
        }

        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(nss.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(makeCollModCmdObj(collModCmd, oldCollOptions, indexInfo));
        oplogEntry.setObject2(o2Builder.done());
        logOperation(opCtx, &oplogEntry);
    }

    // Make sure the UUID values in the Collection metadata, the Collection object, and the UUID
    // catalog are all present and equal.
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.db());
    // Some unit tests call the op observer on an unregistered Database.
    if (!db) {
        return;
    }
    const CollectionPtr& coll =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);

    invariant(coll->uuid() == uuid);
    invariant(DurableCatalog::get(opCtx)->isEqualToMetadataUUID(opCtx, coll->getCatalogId(), uuid));
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss({dbName, "$cmd"});
    oplogEntry.setObject(BSON("dropDatabase" << 1));
    logOperation(opCtx, &oplogEntry);

    uassert(
        50714, "dropping the admin database is not allowed.", dbName != NamespaceString::kAdminDb);

    if (dbName == NamespaceString::kSessionTransactionsTableNamespace.db()) {
        MongoDSessionCatalog::invalidateAllSessions(opCtx);
    }
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType) {
    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications.
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(collectionName.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(BSON("drop" << collectionName.coll()));
        oplogEntry.setObject2(makeObject2ForDropOrRename(numRecords));
        logOperation(opCtx, &oplogEntry);
    }

    uassert(50715,
            "dropping the server configuration collection (admin.system.version) is not allowed.",
            collectionName != NamespaceString::kServerConfigurationNamespace);

    if (collectionName.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onSystemViewsCollectionDrop(opCtx, collectionName);
    } else if (collectionName == NamespaceString::kSessionTransactionsTableNamespace) {
        // Disallow this drop if there are currently prepared transactions.
        const auto sessionCatalog = SessionCatalog::get(opCtx);
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        bool noPreparedTxns = true;
        sessionCatalog->scanSessions(matcherAllSessions, [&](const ObservableSession& session) {
            auto txnParticipant = TransactionParticipant::get(session);
            if (txnParticipant.transactionIsPrepared()) {
                noPreparedTxns = false;
            }
        });
        uassert(4852500,
                "Unable to drop transactions table (config.transactions) while prepared "
                "transactions are present.",
                noPreparedTxns);

        MongoDSessionCatalog::invalidateAllSessions(opCtx);
    } else if (collectionName == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).invalidate();
    }

    return {};
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OptionalCollectionUUID uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(BSON("dropIndexes" << nss.coll() << "index" << indexName));
    oplogEntry.setObject2(indexInfo);
    logOperation(opCtx, &oplogEntry);
}


repl::OpTime OpObserverImpl::preRenameCollection(OperationContext* const opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 OptionalCollectionUUID uuid,
                                                 OptionalCollectionUUID dropTargetUUID,
                                                 std::uint64_t numRecords,
                                                 bool stayTemp) {
    BSONObjBuilder builder;
    builder.append("renameCollection", fromCollection.ns());
    builder.append("to", toCollection.ns());
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(fromCollection.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(builder.done());
    if (dropTargetUUID)
        oplogEntry.setObject2(makeObject2ForDropOrRename(numRecords));
    logOperation(opCtx, &oplogEntry);

    return {};
}

void OpObserverImpl::postRenameCollection(OperationContext* const opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          OptionalCollectionUUID uuid,
                                          OptionalCollectionUUID dropTargetUUID,
                                          bool stayTemp) {
    if (fromCollection.isSystemDotViews())
        DurableViewCatalog::onExternalChange(opCtx, fromCollection);
    if (toCollection.isSystemDotViews())
        DurableViewCatalog::onExternalChange(opCtx, toCollection);
}

void OpObserverImpl::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        OptionalCollectionUUID uuid,
                                        OptionalCollectionUUID dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp) {
    preRenameCollection(
        opCtx, fromCollection, toCollection, uuid, dropTargetUUID, numRecords, stayTemp);
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
    ImportCollectionOplogEntry importCollection(
        nss, importUUID, numRecords, dataSize, catalogEntry, storageMetadata, isDryRun);

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setObject(importCollection.toBSON());
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss({dbName, "$cmd"});
    oplogEntry.setObject(applyOpCmd);
    logOperation(opCtx, &oplogEntry);
}

void OpObserverImpl::onEmptyCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   OptionalCollectionUUID uuid) {
    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(collectionName.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(BSON("emptycapped" << collectionName.coll()));
        logOperation(opCtx, &oplogEntry);
    }
}

namespace {
// Accepts an empty BSON builder and appends the given transaction statements to an 'applyOps' array
// field. Appends as many operations as possible until either the constructed object exceeds the
// 16MB limit or the maximum number of transaction statements allowed in one entry.
//
// Returns an iterator to the first statement that wasn't packed into the applyOps object.
std::vector<repl::ReplOperation>::iterator packTransactionStatementsForApplyOps(
    BSONObjBuilder* applyOpsBuilder,
    std::vector<repl::ReplOperation>::iterator stmtBegin,
    std::vector<repl::ReplOperation>::iterator stmtEnd) {

    std::vector<repl::ReplOperation>::iterator stmtIter;
    BSONArrayBuilder opsArray(applyOpsBuilder->subarrayStart("applyOps"_sd));
    for (stmtIter = stmtBegin; stmtIter != stmtEnd; stmtIter++) {
        const auto& stmt = *stmtIter;
        // Stop packing when either number of transaction operations is reached, or when the next
        // one would put the array over the maximum BSON Object User Size.  We rely on the
        // head room between BSONObjMaxUserSize and BSONObjMaxInternalSize to cover the
        // BSON overhead and the other applyOps fields.  But if the array with a single operation
        // exceeds BSONObjMaxUserSize, we still log it, as a single max-length operation
        // should be able to be applied.
        if (opsArray.arrSize() == gMaxNumberOfTransactionOperationsInSingleOplogEntry ||
            (opsArray.arrSize() > 0 &&
             (opsArray.len() + DurableOplogEntry::getDurableReplOperationSize(stmt) >
              BSONObjMaxUserSize)))
            break;
        opsArray.append(stmt.toBSON());
    }
    try {
        // BSONArrayBuilder will throw a BSONObjectTooLarge exception if we exceeded the max BSON
        // size.
        opsArray.done();
    } catch (const AssertionException& e) {
        // Change the error code to TransactionTooLarge if it is BSONObjectTooLarge.
        uassert(ErrorCodes::TransactionTooLarge,
                e.reason(),
                e.code() != ErrorCodes::BSONObjectTooLarge);
        throw;
    }
    return stmtIter;
}

// Logs one applyOps entry and may update the transactions table. Assumes that the given BSON
// builder object already has  an 'applyOps' field appended pointing to the desired array of ops
// i.e. { "applyOps" : [op1, op2, ...] }
//
// @param txnState the 'state' field of the transaction table entry update.
// @param startOpTime the optime of the 'startOpTime' field of the transaction table entry update.
// If boost::none, no 'startOpTime' field will be included in the new transaction table entry. Only
// meaningful if 'updateTxnTable' is true.
// @param updateTxnTable determines whether the transactions table will updated after the oplog
// entry is written.
//
// Returns the optime of the written oplog entry.
OpTimeBundle logApplyOpsForTransaction(OperationContext* opCtx,
                                       MutableOplogEntry* oplogEntry,
                                       boost::optional<DurableTxnStateEnum> txnState,
                                       boost::optional<repl::OpTime> startOpTime,
                                       const bool updateTxnTable) {
    oplogEntry->setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry->setNss({"admin", "$cmd"});
    oplogEntry->setSessionId(opCtx->getLogicalSessionId());
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());

    try {
        OpTimeBundle times;
        times.writeOpTime = logOperation(opCtx, oplogEntry);
        times.wallClockTime = oplogEntry->getWallClockTime();
        if (updateTxnTable) {
            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setLastWriteOpTime(times.writeOpTime);
            sessionTxnRecord.setLastWriteDate(times.wallClockTime);
            sessionTxnRecord.setState(txnState);
            sessionTxnRecord.setStartOpTime(startOpTime);
            onWriteOpCompleted(opCtx, {}, sessionTxnRecord);
        }
        return times;
    } catch (const AssertionException& e) {
        // Change the error code to TransactionTooLarge if it is BSONObjectTooLarge.
        uassert(ErrorCodes::TransactionTooLarge,
                e.reason(),
                e.code() != ErrorCodes::BSONObjectTooLarge);
        throw;
    }
    MONGO_UNREACHABLE;
}

// Logs transaction oplog entries for preparing a transaction or committing an unprepared
// transaction. This includes the in-progress 'partialTxn' oplog entries followed by the implicit
// prepare or commit entry. If the 'prepare' argument is true, it will log entries for a prepared
// transaction. Otherwise, it logs entries for an unprepared transaction. The total number of oplog
// entries written will be <= the size of the given 'stmts' vector, and will depend on how many
// transaction statements are given, the data size of each statement, and the
// 'maxNumberOfTransactionOperationsInSingleOplogEntry' server parameter.
//
// This function expects that the size of 'oplogSlots' be at least as big as the size of 'stmts' in
// the worst case, where each operation requires an applyOps entry of its own. If there are more
// oplog slots than applyOps operations are written, the number of oplog slots corresponding to the
// number of applyOps written will be used. It also expects that the vector of given statements is
// non-empty.
//
// In the case of writing entries for a prepared transaction, the last oplog entry (i.e. the
// implicit prepare) will always be written using the last oplog slot given, even if this means
// skipping over some reserved slots.
//
// The number of oplog entries written is returned.
int logOplogEntriesForTransaction(OperationContext* opCtx,
                                  std::vector<repl::ReplOperation>* stmts,
                                  const std::vector<OplogSlot>& oplogSlots,
                                  size_t numberOfPreImagesToWrite,
                                  bool prepare) {
    invariant(!stmts->empty());
    invariant(stmts->size() <= oplogSlots.size());

    // Storage transaction commit is the last place inside a transaction that can throw an
    // exception. In order to safely allow exceptions to be thrown at that point, this function must
    // be called from an outer WriteUnitOfWork in order to be rolled back upon reaching the
    // exception.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    const auto txnParticipant = TransactionParticipant::get(opCtx);
    OpTimeBundle prevWriteOpTime;
    auto numEntriesWritten = 0;

    // Writes to the oplog only require a Global intent lock. Guaranteed by
    // OplogSlotReserver.
    invariant(opCtx->lockState()->isWriteLocked());

    prevWriteOpTime.writeOpTime = txnParticipant.getLastWriteOpTime();
    auto currOplogSlot = oplogSlots.begin();
    // We never want to store pre-images when we're migrating oplog entries from another
    // replica set.
    const auto& migrationRecipientInfo = repl::tenantMigrationRecipientInfo(opCtx);

    if (numberOfPreImagesToWrite > 0 && !migrationRecipientInfo) {
        for (auto& statement : *stmts) {
            if (statement.getPreImage().isEmpty()) {
                continue;
            }

            auto slot = *currOplogSlot;
            ++currOplogSlot;

            MutableOplogEntry preImageEntry;
            preImageEntry.setOpType(repl::OpTypeEnum::kNoop);
            preImageEntry.setObject(statement.getPreImage());
            preImageEntry.setNss(statement.getNss());
            preImageEntry.setUuid(statement.getUuid());
            preImageEntry.setOpTime(slot);

            auto opTime = logOperation(opCtx, &preImageEntry);
            statement.setPreImageOpTime(opTime);
        }
    }

    // At the beginning of each loop iteration below, 'stmtsIter' will always point to the
    // first statement of the sequence of remaining, unpacked transaction statements. If all
    // statements have been packed, it should point to stmts.end(), which is the loop's
    // termination condition.
    auto stmtsIter = stmts->begin();
    while (stmtsIter != stmts->end()) {

        BSONObjBuilder applyOpsBuilder;
        auto nextStmt =
            packTransactionStatementsForApplyOps(&applyOpsBuilder, stmtsIter, stmts->end());

        // If we packed the last op, then the next oplog entry we log should be the implicit
        // commit or implicit prepare, i.e. we omit the 'partialTxn' field.
        auto firstOp = stmtsIter == stmts->begin();
        auto lastOp = nextStmt == stmts->end();

        auto implicitCommit = lastOp && !prepare;
        auto implicitPrepare = lastOp && prepare;
        auto isPartialTxn = !lastOp;
        if (isPartialTxn) {
            // Partial transactions create multiple oplog entries in the same WriteUnitOfWork.
            // Because of this, partial transactions will set multiple timestamps, violating the
            // multi timestamp constraint. It's safe to ignore the multi timestamp constraints here
            // as additional rollback logic is in place for this case.
            opCtx->recoveryUnit()->ignoreAllMultiTimestampConstraints();
        }

        // A 'prepare' oplog entry should never include a 'partialTxn' field.
        invariant(!(isPartialTxn && implicitPrepare));
        if (implicitPrepare) {
            applyOpsBuilder.append("prepare", true);
        }
        if (isPartialTxn) {
            applyOpsBuilder.append("partialTxn", true);
        }

        // The 'count' field gives the total number of individual operations in the
        // transaction, and is included on a non-initial implicit commit or prepare entry.
        if (lastOp && !firstOp) {
            applyOpsBuilder.append("count", static_cast<long long>(stmts->size()));
        }

        // For both prepared and unprepared transactions, update the transactions table on
        // the first and last op.
        auto updateTxnTable = firstOp || lastOp;

        // Use the next reserved oplog slot. In the special case of writing the implicit
        // 'prepare' oplog entry, we use the last reserved oplog slot, since callers of this
        // function will expect that timestamp to be used as the 'prepare' timestamp. This
        // may mean we skipped over some reserved slots, but there's no harm in that.
        auto oplogSlot = implicitPrepare ? oplogSlots.back() : *currOplogSlot++;

        // The first optime of the transaction is always the first oplog slot, except in the
        // case of a single prepare oplog entry.
        auto firstOpTimeOfTxn =
            (implicitPrepare && firstOp) ? oplogSlots.back() : oplogSlots.front();

        // We always write the startOpTime field, which is the first optime of the
        // transaction, except when transitioning to 'committed' state, in which it should
        // no longer be set.
        auto startOpTime = boost::make_optional(!implicitCommit, firstOpTimeOfTxn);

        MutableOplogEntry oplogEntry;
        oplogEntry.setOpTime(oplogSlot);
        oplogEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTime.writeOpTime);
        oplogEntry.setObject(applyOpsBuilder.done());
        auto txnState = isPartialTxn
            ? DurableTxnStateEnum::kInProgress
            : (implicitPrepare ? DurableTxnStateEnum::kPrepared : DurableTxnStateEnum::kCommitted);
        prevWriteOpTime =
            logApplyOpsForTransaction(opCtx, &oplogEntry, txnState, startOpTime, updateTxnTable);

        hangAfterLoggingApplyOpsForTransaction.pauseWhileSet();

        // Advance the iterator to the beginning of the remaining unpacked statements.
        stmtsIter = nextStmt;
        numEntriesWritten++;
    }

    return numEntriesWritten;
}

void logCommitOrAbortForPreparedTransaction(OperationContext* opCtx,
                                            MutableOplogEntry* oplogEntry,
                                            DurableTxnStateEnum durableState) {
    oplogEntry->setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry->setNss({"admin", "$cmd"});
    oplogEntry->setSessionId(opCtx->getLogicalSessionId());
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());
    oplogEntry->setPrevWriteOpTimeInTransaction(
        TransactionParticipant::get(opCtx).getLastWriteOpTime());

    // There should not be a parent WUOW outside of this one. This guarantees the safety of the
    // write conflict retry loop.
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // We must not have a maximum lock timeout, since writing the commit or abort oplog entry for a
    // prepared transaction must always succeed.
    invariant(!opCtx->lockState()->hasMaxLockTimeout());

    writeConflictRetry(
        opCtx, "onPreparedTransactionCommitOrAbort", NamespaceString::kRsOplogNamespace.ns(), [&] {
            // Writes to the oplog only require a Global intent lock. Guaranteed by
            // OplogSlotReserver.
            invariant(opCtx->lockState()->isWriteLocked());

            WriteUnitOfWork wuow(opCtx);
            const auto oplogOpTime = logOperation(opCtx, oplogEntry);
            invariant(oplogEntry->getOpTime().isNull() || oplogEntry->getOpTime() == oplogOpTime);

            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setLastWriteOpTime(oplogOpTime);
            sessionTxnRecord.setLastWriteDate(oplogEntry->getWallClockTime());
            sessionTxnRecord.setState(durableState);
            onWriteOpCompleted(opCtx, {}, sessionTxnRecord);
            wuow.commit();
        });
}

}  //  namespace

void OpObserverImpl::onUnpreparedTransactionCommit(OperationContext* opCtx,
                                                   std::vector<repl::ReplOperation>* statements,
                                                   size_t numberOfPreImagesToWrite) {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // It is possible that the transaction resulted in no changes.  In that case, we should
    // not write an empty applyOps entry.
    if (statements->empty())
        return;

    repl::OpTime commitOpTime;
    // Reserve all the optimes in advance, so we only need to get the optime mutex once.  We
    // reserve enough entries for all statements in the transaction.
    auto oplogSlots = repl::getNextOpTimes(opCtx, statements->size() + numberOfPreImagesToWrite);

    // Throw TenantMigrationConflict error if the database for the transaction statements is being
    // migrated. We only need check the namespace of the first statement since a transaction's
    // statements must all be for the same tenant.
    tenant_migration_access_blocker::onWriteToDatabase(opCtx, statements->begin()->getNss().db());

    if (MONGO_unlikely(hangAndFailUnpreparedCommitAfterReservingOplogSlot.shouldFail())) {
        hangAndFailUnpreparedCommitAfterReservingOplogSlot.pauseWhileSet(opCtx);
        uasserted(51268, "hangAndFailUnpreparedCommitAfterReservingOplogSlot fail point enabled");
    }

    // Log in-progress entries for the transaction along with the implicit commit.
    int numOplogEntries = logOplogEntriesForTransaction(
        opCtx, statements, oplogSlots, numberOfPreImagesToWrite, false);
    commitOpTime = oplogSlots[numOplogEntries - 1];
    invariant(!commitOpTime.isNull());
    shardObserveTransactionPrepareOrUnpreparedCommit(opCtx, *statements, commitOpTime);
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

    logCommitOrAbortForPreparedTransaction(opCtx, &oplogEntry, DurableTxnStateEnum::kCommitted);
}

void OpObserverImpl::onTransactionPrepare(OperationContext* opCtx,
                                          const std::vector<OplogSlot>& reservedSlots,
                                          std::vector<repl::ReplOperation>* statements,
                                          size_t numberOfPreImagesToWrite) {
    invariant(!reservedSlots.empty());
    const auto prepareOpTime = reservedSlots.back();
    invariant(opCtx->getTxnNumber());
    invariant(!prepareOpTime.isNull());

    // Don't write oplog entry on secondaries.
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    {
        // We should have reserved enough slots.
        invariant(reservedSlots.size() >= statements->size());
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

        writeConflictRetry(
            opCtx, "onTransactionPrepare", NamespaceString::kRsOplogNamespace.ns(), [&] {
                // Writes to the oplog only require a Global intent lock. Guaranteed by
                // OplogSlotReserver.
                invariant(opCtx->lockState()->isWriteLocked());

                WriteUnitOfWork wuow(opCtx);
                // It is possible that the transaction resulted in no changes, In that case, we
                // should not write any operations other than the prepare oplog entry.
                if (!statements->empty()) {
                    // We had reserved enough oplog slots for the worst case where each operation
                    // produced one oplog entry.  When operations are smaller and can be packed, we
                    // will waste the extra slots.  The implicit prepare oplog entry will still use
                    // the last reserved slot, because the transaction participant has already used
                    // that as the prepare time.
                    logOplogEntriesForTransaction(opCtx,
                                                  statements,
                                                  reservedSlots,
                                                  numberOfPreImagesToWrite,
                                                  true /* prepare */);

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
                    oplogEntry.setOpTime(oplogSlot);
                    oplogEntry.setPrevWriteOpTimeInTransaction(repl::OpTime());
                    oplogEntry.setObject(applyOpsBuilder.done());
                    logApplyOpsForTransaction(opCtx,
                                              &oplogEntry,
                                              DurableTxnStateEnum::kPrepared,
                                              oplogSlot,
                                              true /* updateTxnTable */);
                }
                wuow.commit();
            });
    }

    shardObserveTransactionPrepareOrUnpreparedCommit(opCtx, *statements, prepareOpTime);
}

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

    logCommitOrAbortForPreparedTransaction(opCtx, &oplogEntry, DurableTxnStateEnum::kAborted);
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

    // Force the config server to update its shard registry on next access. Otherwise it may have
    // the stale data that has been just rolled back.
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        if (auto shardRegistry = Grid::get(opCtx)->shardRegistry()) {
            shardRegistry->clearEntries();
        }
    }

    // Force the default read/write concern cache to reload on next access in case the defaults
    // document was rolled back.
    ReadWriteConcernDefaults::get(opCtx).invalidate();
}

}  // namespace mongo
