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

#include "mongo/platform/basic.h"

#include "mongo/db/op_observer_impl.h"

#include <limits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
using repl::OplogEntry;

// This is a startup server parameter defined in transaction_participant.cpp.
extern bool useMultipleOplogEntryFormatForTransactions;

namespace {

MONGO_FAIL_POINT_DEFINE(failCollectionUpdates);

const auto documentKeyDecoration = OperationContext::declareDecoration<BSONObj>();

constexpr auto kNumRecordsFieldName = "numRecords"_sd;
constexpr auto kMsgFieldName = "msg"_sd;
constexpr long long kInvalidNumRecords = -1LL;

repl::OpTime logOperation(OperationContext* opCtx,
                          const char* opstr,
                          const NamespaceString& ns,
                          OptionalCollectionUUID uuid,
                          const BSONObj& obj,
                          const BSONObj* o2,
                          bool fromMigrate,
                          Date_t wallClockTime,
                          const OperationSessionInfo& sessionInfo,
                          StmtId stmtId,
                          const repl::OplogLink& oplogLink,
                          bool prepare,
                          const OplogSlot& oplogSlot) {
    auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
    auto opTime = repl::logOp(opCtx,
                              opstr,
                              ns,
                              uuid,
                              obj,
                              o2,
                              fromMigrate,
                              wallClockTime,
                              sessionInfo,
                              stmtId,
                              oplogLink,
                              prepare,
                              oplogSlot);

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
                        const NamespaceString& nss,
                        std::vector<StmtId> stmtIdsWritten,
                        const repl::OpTime& lastStmtIdWriteOpTime,
                        Date_t lastStmtIdWriteDate,
                        boost::optional<DurableTxnStateEnum> txnState) {
    if (lastStmtIdWriteOpTime.isNull())
        return;

    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (!txnParticipant)
        return;

    txnParticipant.onWriteOpCompletedOnPrimary(opCtx,
                                               *opCtx->getTxnNumber(),
                                               std::move(stmtIdsWritten),
                                               lastStmtIdWriteOpTime,
                                               lastStmtIdWriteDate,
                                               txnState);
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

Date_t getWallClockTimeForOpLog(OperationContext* opCtx) {
    auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
    return clockSource->now();
}

struct OpTimeBundle {
    repl::OpTime writeOpTime;
    repl::OpTime prePostImageOpTime;
    Date_t wallClockTime;
};

/**
 * Write oplog entry(ies) for the update operation.
 */
OpTimeBundle replLogUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    BSONObj storeObj;
    if (args.updateArgs.storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
        invariant(args.updateArgs.preImageDoc);
        storeObj = *args.updateArgs.preImageDoc;
    } else if (args.updateArgs.storeDocOption == CollectionUpdateArgs::StoreDocOption::PostImage) {
        storeObj = args.updateArgs.updatedDoc;
    }

    OperationSessionInfo sessionInfo;
    repl::OplogLink oplogLink;

    const auto txnParticipant = TransactionParticipant::get(opCtx);
    if (txnParticipant) {
        sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
        sessionInfo.setTxnNumber(*opCtx->getTxnNumber());
        oplogLink.prevOpTime = txnParticipant.getLastWriteOpTime();
    }

    OpTimeBundle opTimes;
    opTimes.wallClockTime = getWallClockTimeForOpLog(opCtx);

    if (!storeObj.isEmpty() && opCtx->getTxnNumber()) {
        auto noteUpdateOpTime = logOperation(opCtx,
                                             "n",
                                             args.nss,
                                             args.uuid,
                                             storeObj,
                                             nullptr,
                                             false,
                                             opTimes.wallClockTime,
                                             sessionInfo,
                                             args.updateArgs.stmtId,
                                             {},
                                             false /* prepare */,
                                             OplogSlot());

        opTimes.prePostImageOpTime = noteUpdateOpTime;

        if (args.updateArgs.storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
            oplogLink.preImageOpTime = noteUpdateOpTime;
        } else if (args.updateArgs.storeDocOption ==
                   CollectionUpdateArgs::StoreDocOption::PostImage) {
            oplogLink.postImageOpTime = noteUpdateOpTime;
        }
    }

    opTimes.writeOpTime = logOperation(opCtx,
                                       "u",
                                       args.nss,
                                       args.uuid,
                                       args.updateArgs.update,
                                       &args.updateArgs.criteria,
                                       args.updateArgs.fromMigrate,
                                       opTimes.wallClockTime,
                                       sessionInfo,
                                       args.updateArgs.stmtId,
                                       oplogLink,
                                       false /* prepare */,
                                       OplogSlot());

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
    OperationSessionInfo sessionInfo;
    repl::OplogLink oplogLink;

    const auto txnParticipant = TransactionParticipant::get(opCtx);
    if (txnParticipant) {
        sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
        sessionInfo.setTxnNumber(*opCtx->getTxnNumber());
        oplogLink.prevOpTime = txnParticipant.getLastWriteOpTime();
    }

    OpTimeBundle opTimes;
    opTimes.wallClockTime = getWallClockTimeForOpLog(opCtx);

    if (deletedDoc && opCtx->getTxnNumber()) {
        auto noteOplog = logOperation(opCtx,
                                      "n",
                                      nss,
                                      uuid,
                                      deletedDoc.get(),
                                      nullptr,
                                      false,
                                      opTimes.wallClockTime,
                                      sessionInfo,
                                      stmtId,
                                      {},
                                      false /* prepare */,
                                      OplogSlot());
        opTimes.prePostImageOpTime = noteOplog;
        oplogLink.preImageOpTime = noteOplog;
    }

    auto& documentKey = documentKeyDecoration(opCtx);
    opTimes.writeOpTime = logOperation(opCtx,
                                       "d",
                                       nss,
                                       uuid,
                                       documentKey,
                                       nullptr,
                                       fromMigrate,
                                       opTimes.wallClockTime,
                                       sessionInfo,
                                       stmtId,
                                       oplogLink,
                                       false /* prepare */,
                                       OplogSlot());
    return opTimes;
}

/**
 * Write oplog entry for applyOps/atomic transaction operations.
 */
OpTimeBundle replLogApplyOps(OperationContext* opCtx,
                             const NamespaceString& cmdNss,
                             const BSONObj& applyOpCmd,
                             const OperationSessionInfo& sessionInfo,
                             StmtId stmtId,
                             const repl::OplogLink& oplogLink,
                             bool prepare,
                             const OplogSlot& oplogSlot) {
    OpTimeBundle times;
    times.wallClockTime = getWallClockTimeForOpLog(opCtx);
    times.writeOpTime = logOperation(opCtx,
                                     "c",
                                     cmdNss,
                                     {},
                                     applyOpCmd,
                                     nullptr,
                                     false,
                                     times.wallClockTime,
                                     sessionInfo,
                                     stmtId,
                                     oplogLink,
                                     prepare,
                                     oplogSlot);
    return times;
}

}  // namespace

BSONObj OpObserverImpl::getDocumentKey(OperationContext* opCtx,
                                       NamespaceString const& nss,
                                       BSONObj const& doc) {
    auto metadata = CollectionShardingState::get(opCtx, nss)->getMetadataForOperation(opCtx);
    return metadata->extractDocumentKey(doc).getOwned();
}

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   CollectionUUID uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {

    BSONObjBuilder builder;
    builder.append("createIndexes", nss.coll());

    for (const auto& e : indexDoc) {
        if (e.fieldNameStringData() != "ns"_sd)
            builder.append(e);
    }

    logOperation(opCtx,
                 "c",
                 nss.getCommandNS(),
                 uuid,
                 builder.done(),
                 nullptr,
                 fromMigrate,
                 getWallClockTimeForOpLog(opCtx),
                 {},
                 kUninitializedStmtId,
                 {},
                 false /* prepare */,
                 OplogSlot());
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
        BSONObjBuilder builder;
        for (const auto& e : indexDoc) {
            if (e.fieldNameStringData() != "ns"_sd)
                builder.append(e);
        }
        indexesArr.append(builder.obj());
    }
    indexesArr.done();

    logOperation(opCtx,
                 "c",
                 nss.getCommandNS(),
                 collUUID,
                 oplogEntryBuilder.done(),
                 nullptr,
                 fromMigrate,
                 getWallClockTimeForOpLog(opCtx),
                 {},
                 kUninitializedStmtId,
                 {},
                 false /* prepare */,
                 OplogSlot());
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
        BSONObjBuilder builder;
        for (const auto& e : indexDoc) {
            if (e.fieldNameStringData() != "ns"_sd)
                builder.append(e);
        }
        indexesArr.append(builder.obj());
    }
    indexesArr.done();

    logOperation(opCtx,
                 "c",
                 nss.getCommandNS(),
                 collUUID,
                 oplogEntryBuilder.done(),
                 nullptr,
                 fromMigrate,
                 getWallClockTimeForOpLog(opCtx),
                 {},
                 kUninitializedStmtId,
                 {},
                 false /* prepare */,
                 OplogSlot());
}

void OpObserverImpl::onAbortIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       CollectionUUID collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("abortIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (auto indexDoc : indexes) {
        BSONObjBuilder builder;
        for (const auto& e : indexDoc) {
            if (e.fieldNameStringData() != "ns"_sd)
                builder.append(e);
        }
        indexesArr.append(builder.obj());
    }
    indexesArr.done();

    logOperation(opCtx,
                 "c",
                 nss.getCommandNS(),
                 collUUID,
                 oplogEntryBuilder.done(),
                 nullptr,
                 fromMigrate,
                 getWallClockTimeForOpLog(opCtx),
                 {},
                 kUninitializedStmtId,
                 {},
                 false /* prepare */,
                 OplogSlot());
}

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               bool fromMigrate) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction = txnParticipant && opCtx->writesAreReplicated() &&
        txnParticipant.inMultiDocumentTransaction();

    Date_t lastWriteDate;

    std::vector<repl::OpTime> opTimeList;
    repl::OpTime lastOpTime;

    if (inMultiDocumentTransaction) {
        // Do not add writes to the profile collection to the list of transaction operations, since
        // these are done outside the transaction.
        if (!opCtx->getWriteUnitOfWork()) {
            invariant(nss.isSystemDotProfile());
            return;
        }
        for (auto iter = first; iter != last; iter++) {
            auto operation = OplogEntry::makeInsertOperation(nss, uuid, iter->doc);
            txnParticipant.addTransactionOperation(opCtx, operation);
        }
    } else {
        lastWriteDate = getWallClockTimeForOpLog(opCtx);
        opTimeList = repl::logInsertOps(opCtx, nss, uuid, first, last, fromMigrate, lastWriteDate);
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

        onWriteOpCompleted(opCtx, nss, stmtIdsWritten, lastOpTime, lastWriteDate, boost::none);
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
    } else if (nss == NamespaceString::kServerConfigurationNamespace) {
        // We must check server configuration collection writes for featureCompatibilityVersion
        // document changes.
        for (auto it = first; it != last; it++) {
            FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, it->doc);
        }
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace && !lastOpTime.isNull()) {
        for (auto it = first; it != last; it++) {
            MongoDSessionCatalog::invalidateSessions(opCtx, it->doc);
        }
    }
}

void OpObserverImpl::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    MONGO_FAIL_POINT_BLOCK(failCollectionUpdates, extraData) {
        auto collElem = extraData.getData()["collectionNS"];
        // If the failpoint specifies no collection or matches the existing one, fail.
        if (!collElem || args.nss.ns() == collElem.String()) {
            uasserted(40654,
                      str::stream() << "failCollectionUpdates failpoint enabled, namespace: "
                                    << args.nss.ns()
                                    << ", update: "
                                    << args.updateArgs.update
                                    << " on document with "
                                    << args.updateArgs.criteria);
        }
    }

    // Do not log a no-op operation; see SERVER-21738
    if (args.updateArgs.update.isEmpty()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction = txnParticipant && opCtx->writesAreReplicated() &&
        txnParticipant.inMultiDocumentTransaction();

    OpTimeBundle opTime;
    if (inMultiDocumentTransaction) {
        auto operation = OplogEntry::makeUpdateOperation(
            args.nss, args.uuid, args.updateArgs.update, args.updateArgs.criteria);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        opTime = replLogUpdate(opCtx, args);
        onWriteOpCompleted(opCtx,
                           args.nss,
                           std::vector<StmtId>{args.updateArgs.stmtId},
                           opTime.writeOpTime,
                           opTime.wallClockTime,
                           boost::none);
    }

    if (args.nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!args.updateArgs.fromMigrate) {
            shardObserveUpdateOp(opCtx,
                                 args.nss,
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
    } else if (args.nss == NamespaceString::kServerConfigurationNamespace) {
        // We must check server configuration collection writes for featureCompatibilityVersion
        // document changes.
        FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, args.updateArgs.updatedDoc);
    } else if (args.nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        MongoDSessionCatalog::invalidateSessions(opCtx, args.updateArgs.updatedDoc);
    }
}

void OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                   NamespaceString const& nss,
                                   BSONObj const& doc) {
    documentKeyDecoration(opCtx) = getDocumentKey(opCtx, nss, doc);

    shardObserveAboutToDelete(opCtx, nss, doc);
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const NamespaceString& nss,
                              OptionalCollectionUUID uuid,
                              StmtId stmtId,
                              bool fromMigrate,
                              const boost::optional<BSONObj>& deletedDoc) {
    auto& documentKey = documentKeyDecoration(opCtx);
    invariant(!documentKey.isEmpty());

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction = txnParticipant && opCtx->writesAreReplicated() &&
        txnParticipant.inMultiDocumentTransaction();

    OpTimeBundle opTime;
    if (inMultiDocumentTransaction) {
        auto operation =
            OplogEntry::makeDeleteOperation(nss, uuid, deletedDoc ? deletedDoc.get() : documentKey);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        opTime = replLogDelete(opCtx, nss, uuid, stmtId, fromMigrate, deletedDoc);
        onWriteOpCompleted(opCtx,
                           nss,
                           std::vector<StmtId>{stmtId},
                           opTime.writeOpTime,
                           opTime.wallClockTime,
                           boost::none);
    }

    if (nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!fromMigrate) {
            shardObserveDeleteOp(opCtx,
                                 nss,
                                 documentKey,
                                 opTime.writeOpTime,
                                 opTime.prePostImageOpTime,
                                 inMultiDocumentTransaction);
        }
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    } else if (nss.isServerConfigurationCollection()) {
        auto _id = documentKey["_id"];
        if (_id.type() == BSONType::String &&
            _id.String() == FeatureCompatibilityVersionParser::kParameterName)
            uasserted(40670, "removing FeatureCompatibilityVersion document is not allowed");
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        MongoDSessionCatalog::invalidateSessions(opCtx, documentKey);
    }
}

void OpObserverImpl::onInternalOpMessage(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const boost::optional<UUID> uuid,
                                         const BSONObj& msgObj,
                                         const boost::optional<BSONObj> o2MsgObj) {
    const BSONObj* o2MsgPtr = o2MsgObj ? o2MsgObj.get_ptr() : nullptr;
    logOperation(opCtx,
                 "n",
                 nss,
                 uuid,
                 msgObj,
                 o2MsgPtr,
                 false,
                 getWallClockTimeForOpLog(opCtx),
                 {},
                 kUninitializedStmtId,
                 {},
                 false /* prepare */,
                 OplogSlot());
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        Collection* coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex,
                                        const OplogSlot& createOpTime) {
    const auto cmdNss = collectionName.getCommandNS();

    const auto cmdObj = makeCreateCollCmdObj(collectionName, options, idIndex);

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        logOperation(opCtx,
                     "c",
                     cmdNss,
                     options.uuid,
                     cmdObj,
                     nullptr,
                     false,
                     getWallClockTimeForOpLog(opCtx),
                     {},
                     kUninitializedStmtId,
                     {},
                     false /* prepare */,
                     createOpTime);
    }

    if (options.uuid) {
        opCtx->recoveryUnit()->onRollback([opCtx, collectionName]() {
            NamespaceUUIDCache::get(opCtx).evictNamespace(collectionName);
        });
    }
}

void OpObserverImpl::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<TTLCollModInfo> ttlInfo) {
    const auto cmdNss = nss.getCommandNS();

    // Create the 'o' field object.
    const auto cmdObj = makeCollModCmdObj(collModCmd, oldCollOptions, ttlInfo);

    // Create the 'o2' field object. We save the old collection metadata and TTL expiration.
    BSONObjBuilder o2Builder;
    o2Builder.append("collectionOptions_old", oldCollOptions.toBSON());
    if (ttlInfo) {
        auto oldExpireAfterSeconds = durationCount<Seconds>(ttlInfo->oldExpireAfterSeconds);
        o2Builder.append("expireAfterSeconds_old", oldExpireAfterSeconds);
    }

    const auto o2Obj = o2Builder.done();

    if (!nss.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        logOperation(opCtx,
                     "c",
                     cmdNss,
                     uuid,
                     cmdObj,
                     &o2Obj,
                     false,
                     getWallClockTimeForOpLog(opCtx),
                     {},
                     kUninitializedStmtId,
                     {},
                     false /* prepare */,
                     OplogSlot());
    }

    // Make sure the UUID values in the Collection metadata, the Collection object, and the UUID
    // catalog are all present and equal.
    invariant(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.db());
    // Some unit tests call the op observer on an unregistered Database.
    if (!db) {
        return;
    }
    Collection* coll = db->getCollection(opCtx, nss.ns());

    invariant(coll->uuid());
    invariant(coll->uuid() == uuid);
    CollectionCatalogEntry* entry = coll->getCatalogEntry();
    invariant(entry->isEqualToMetadataUUID(opCtx, uuid));
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    const NamespaceString cmdNss{dbName, "$cmd"};
    const auto cmdObj = BSON("dropDatabase" << 1);

    logOperation(opCtx,
                 "c",
                 cmdNss,
                 {},
                 cmdObj,
                 nullptr,
                 false,
                 getWallClockTimeForOpLog(opCtx),
                 {},
                 kUninitializedStmtId,
                 {},
                 false /* prepare */,
                 OplogSlot());

    uassert(
        50714, "dropping the admin database is not allowed.", dbName != NamespaceString::kAdminDb);

    if (dbName == NamespaceString::kSessionTransactionsTableNamespace.db()) {
        MongoDSessionCatalog::invalidateSessions(opCtx, boost::none);
    }

    NamespaceUUIDCache::get(opCtx).evictNamespacesInDatabase(dbName);
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType) {
    const auto cmdNss = collectionName.getCommandNS();
    const auto cmdObj = BSON("drop" << collectionName.coll());
    const auto obj2 = makeObject2ForDropOrRename(numRecords);

    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications.
        logOperation(opCtx,
                     "c",
                     cmdNss,
                     uuid,
                     cmdObj,
                     &obj2,
                     false,
                     getWallClockTimeForOpLog(opCtx),
                     {},
                     kUninitializedStmtId,
                     {},
                     false /* prepare */,
                     OplogSlot());
    }

    uassert(50715,
            "dropping the server configuration collection (admin.system.version) is not allowed.",
            collectionName != NamespaceString::kServerConfigurationNamespace);

    if (collectionName.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, collectionName);
    } else if (collectionName == NamespaceString::kSessionTransactionsTableNamespace) {
        MongoDSessionCatalog::invalidateSessions(opCtx, boost::none);
    }

    // Evict namespace entry from the namespace/uuid cache if it exists.
    NamespaceUUIDCache::get(opCtx).evictNamespace(collectionName);

    return {};
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OptionalCollectionUUID uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    const auto cmdNss = nss.getCommandNS();
    const auto cmdObj = BSON("dropIndexes" << nss.coll() << "index" << indexName);

    logOperation(opCtx,
                 "c",
                 cmdNss,
                 uuid,
                 cmdObj,
                 &indexInfo,
                 false,
                 getWallClockTimeForOpLog(opCtx),
                 {},
                 kUninitializedStmtId,
                 {},
                 false /* prepare */,
                 OplogSlot());
}


repl::OpTime OpObserverImpl::preRenameCollection(OperationContext* const opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 OptionalCollectionUUID uuid,
                                                 OptionalCollectionUUID dropTargetUUID,
                                                 std::uint64_t numRecords,
                                                 bool stayTemp) {
    const auto cmdNss = fromCollection.getCommandNS();

    BSONObjBuilder builder;
    builder.append("renameCollection", fromCollection.ns());
    builder.append("to", toCollection.ns());
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    const auto cmdObj = builder.done();
    BSONObj obj2;
    BSONObj* obj2Ptr = nullptr;
    if (dropTargetUUID) {
        obj2 = makeObject2ForDropOrRename(numRecords);
        obj2Ptr = &obj2;
    }

    logOperation(opCtx,
                 "c",
                 cmdNss,
                 uuid,
                 cmdObj,
                 obj2Ptr,
                 false,
                 getWallClockTimeForOpLog(opCtx),
                 {},
                 kUninitializedStmtId,
                 {},
                 false /* prepare */,
                 OplogSlot());

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

    // Evict namespace entry from the namespace/uuid cache if it exists.
    NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
    cache.evictNamespace(fromCollection);
    cache.evictNamespace(toCollection);
    opCtx->recoveryUnit()->onRollback(
        [&cache, toCollection]() { cache.evictNamespace(toCollection); });
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

void OpObserverImpl::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    const NamespaceString cmdNss{dbName, "$cmd"};

    // Only transactional 'applyOps' commands can be prepared.
    constexpr bool prepare = false;
    replLogApplyOps(opCtx, cmdNss, applyOpCmd, {}, kUninitializedStmtId, {}, prepare, OplogSlot());
}

void OpObserverImpl::onEmptyCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   OptionalCollectionUUID uuid) {
    const auto cmdNss = collectionName.getCommandNS();
    const auto cmdObj = BSON("emptycapped" << collectionName.coll());

    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications
        logOperation(opCtx,
                     "c",
                     cmdNss,
                     uuid,
                     cmdObj,
                     nullptr,
                     false,
                     getWallClockTimeForOpLog(opCtx),
                     {},
                     kUninitializedStmtId,
                     {},
                     false /* prepare */,
                     OplogSlot());
    }
}

namespace {

OpTimeBundle logApplyOpsForTransaction(OperationContext* opCtx,
                                       std::vector<repl::ReplOperation> stmts,
                                       const OplogSlot& prepareOplogSlot) {
    BSONObjBuilder applyOpsBuilder;
    BSONArrayBuilder opsArray(applyOpsBuilder.subarrayStart("applyOps"_sd));
    for (auto& stmt : stmts) {
        opsArray.append(stmt.toBSON());
    }
    opsArray.done();

    const NamespaceString cmdNss{"admin", "$cmd"};

    OperationSessionInfo sessionInfo;
    repl::OplogLink oplogLink;
    sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
    sessionInfo.setTxnNumber(*opCtx->getTxnNumber());

    auto txnParticipant = TransactionParticipant::get(opCtx);
    oplogLink.prevOpTime = txnParticipant.getLastWriteOpTime();
    // Until we support multiple oplog entries per transaction, prevOpTime should always be null.
    invariant(oplogLink.prevOpTime.isNull());

    try {
        // We are only given an oplog slot for prepared transactions.
        auto prepare = !prepareOplogSlot.opTime.isNull();
        if (prepare) {
            // TODO: SERVER-36814 Remove "prepare" field on applyOps.
            applyOpsBuilder.append("prepare", true);
        }
        auto applyOpCmd = applyOpsBuilder.done();
        const StmtId stmtId(0);

        auto times = replLogApplyOps(
            opCtx, cmdNss, applyOpCmd, sessionInfo, stmtId, oplogLink, prepare, prepareOplogSlot);

        auto txnState = [prepare]() -> boost::optional<DurableTxnStateEnum> {
            if (serverGlobalParams.featureCompatibility.getVersion() <
                ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo42) {
                invariant(!prepare);
                return boost::none;
            }

            return prepare ? DurableTxnStateEnum::kPrepared : DurableTxnStateEnum::kCommitted;
        }();

        onWriteOpCompleted(
            opCtx, cmdNss, {stmtId}, times.writeOpTime, times.wallClockTime, txnState);
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

void logCommitOrAbortForPreparedTransaction(OperationContext* opCtx,
                                            const OplogSlot& oplogSlot,
                                            const BSONObj& objectField,
                                            DurableTxnStateEnum durableState) {
    const NamespaceString cmdNss{"admin", "$cmd"};

    OperationSessionInfo sessionInfo;
    repl::OplogLink oplogLink;
    sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
    sessionInfo.setTxnNumber(*opCtx->getTxnNumber());

    auto txnParticipant = TransactionParticipant::get(opCtx);
    oplogLink.prevOpTime = txnParticipant.getLastWriteOpTime();

    const StmtId stmtId(1);
    const auto wallClockTime = getWallClockTimeForOpLog(opCtx);

    // There should not be a parent WUOW outside of this one. This guarantees the safety of the
    // write conflict retry loop.
    invariant(!opCtx->getWriteUnitOfWork());
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // We must not have a maximum lock timeout, since writing the commit or abort oplog entry for a
    // prepared transaction must always succeed.
    invariant(!opCtx->lockState()->hasMaxLockTimeout());

    writeConflictRetry(
        opCtx, "onPreparedTransactionCommitOrAbort", NamespaceString::kRsOplogNamespace.ns(), [&] {

            // Writes to the oplog only require a Global intent lock.
            Lock::GlobalLock globalLock(opCtx, MODE_IX);

            WriteUnitOfWork wuow(opCtx);
            const auto oplogOpTime = logOperation(opCtx,
                                                  "c",
                                                  cmdNss,
                                                  {} /* uuid */,
                                                  objectField,
                                                  nullptr /* o2 */,
                                                  false /* fromMigrate */,
                                                  wallClockTime,
                                                  sessionInfo,
                                                  stmtId,
                                                  oplogLink,
                                                  false /* prepare */,
                                                  oplogSlot);
            invariant(oplogSlot.opTime.isNull() || oplogSlot.opTime == oplogOpTime);

            onWriteOpCompleted(opCtx, cmdNss, {stmtId}, oplogOpTime, wallClockTime, durableState);
            wuow.commit();
        });
}

}  //  namespace

void OpObserverImpl::onUnpreparedTransactionCommit(
    OperationContext* opCtx, const std::vector<repl::ReplOperation>& statements) {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // It is possible that the transaction resulted in no changes.  In that case, we should
    // not write an empty applyOps entry.
    if (statements.empty())
        return;

    const auto commitOpTime = logApplyOpsForTransaction(opCtx, statements, OplogSlot()).writeOpTime;
    invariant(!commitOpTime.isNull());

    shardObserveTransactionCommit(opCtx, statements, commitOpTime, false);
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

    CommitTransactionOplogObject cmdObj;
    cmdObj.setCommitTimestamp(commitTimestamp);
    logCommitOrAbortForPreparedTransaction(
        opCtx, commitOplogEntryOpTime, cmdObj.toBSON(), DurableTxnStateEnum::kCommitted);

    shardObserveTransactionCommit(opCtx, statements, commitOplogEntryOpTime.opTime, true);
}

void OpObserverImpl::onTransactionPrepare(OperationContext* opCtx,
                                          const OplogSlot& prepareOpTime,
                                          std::vector<repl::ReplOperation>& statements) {
    invariant(opCtx->getTxnNumber());
    invariant(!prepareOpTime.opTime.isNull());

    // Don't write oplog entry on secondaries.
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // We write the oplog entry in a side transaction so that we do not commit the now-prepared
    // transaction.
    // We write an empty 'applyOps' entry if there were no writes to choose a prepare timestamp
    // and allow this transaction to be continued on failover.
    {
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

        writeConflictRetry(
            opCtx, "onTransactionPrepare", NamespaceString::kRsOplogNamespace.ns(), [&] {

                // Writes to the oplog only require a Global intent lock.
                Lock::GlobalLock globalLock(opCtx, MODE_IX);

                WriteUnitOfWork wuow(opCtx);
                logApplyOpsForTransaction(opCtx, statements, prepareOpTime);
                wuow.commit();
            });
    }
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

    AbortTransactionOplogObject cmdObj;
    logCommitOrAbortForPreparedTransaction(
        opCtx, *abortOplogEntryOpTime, cmdObj.toBSON(), DurableTxnStateEnum::kAborted);
}

void OpObserverImpl::onReplicationRollback(OperationContext* opCtx,
                                           const RollbackObserverInfo& rbInfo) {
    // If there were ops rolled back that were part of operations on a session, then invalidate
    // the session cache.
    if (rbInfo.rollbackSessionIds.size() > 0) {
        MongoDSessionCatalog::invalidateSessions(opCtx, boost::none);
    }

    // Reset the key manager cache.
    auto validator = LogicalTimeValidator::get(opCtx);
    if (validator) {
        validator->resetKeyManagerCache();
    }

    // Check if the shard identity document rolled back.
    if (rbInfo.shardIdentityRolledBack) {
        fassertFailedNoTrace(50712);
    }

    // The code below will force the config server to update its shard registry.
    // Otherwise it may have the stale data that has been just rolled back.
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        if (auto shardRegistry = Grid::get(opCtx)->shardRegistry()) {
            auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            ON_BLOCK_EXIT([ argsCopy = readConcernArgs, &readConcernArgs ] {
                readConcernArgs = std::move(argsCopy);
            });
            readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
            shardRegistry->reload(opCtx);
        }
    }
}

}  // namespace mongo
