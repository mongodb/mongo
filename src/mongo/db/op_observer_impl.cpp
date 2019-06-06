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
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
using repl::OplogEntry;

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
                          boost::optional<StmtId> stmtId,
                          const repl::OplogLink& oplogLink,
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
                        boost::optional<DurableTxnStateEnum> txnState,
                        boost::optional<repl::OpTime> startOpTime) {
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
                                               txnState,
                                               startOpTime);
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
                             boost::optional<StmtId> stmtId,
                             const repl::OplogLink& oplogLink,
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
                                     oplogSlot);
    return times;
}

}  // namespace

BSONObj OpObserverImpl::getDocumentKey(OperationContext* opCtx,
                                       NamespaceString const& nss,
                                       BSONObj const& doc) {
    auto metadata = CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
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

        onWriteOpCompleted(opCtx,
                           nss,
                           stmtIdsWritten,
                           lastOpTime,
                           lastWriteDate,
                           boost::none,
                           boost::none /* startOpTime */);
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
            MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx, it->doc);
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
                           boost::none,
                           boost::none /* startOpTime */);
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
    } else if (args.nss == NamespaceString::kServerConfigurationNamespace) {
        // We must check server configuration collection writes for featureCompatibilityVersion
        // document changes.
        FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, args.updateArgs.updatedDoc);
    } else if (args.nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx,
                                                                     args.updateArgs.updatedDoc);
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
                           boost::none,
                           boost::none /* startOpTime */);
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
        MongoDSessionCatalog::observeDirectWriteToConfigTransactions(opCtx, documentKey);
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
                     createOpTime);
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
    Collection* coll = db->getCollection(opCtx, nss);

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
                 OplogSlot());

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
                     OplogSlot());
    }

    uassert(50715,
            "dropping the server configuration collection (admin.system.version) is not allowed.",
            collectionName != NamespaceString::kServerConfigurationNamespace);

    if (collectionName.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, collectionName);
    } else if (collectionName == NamespaceString::kSessionTransactionsTableNamespace) {
        MongoDSessionCatalog::invalidateAllSessions(opCtx);
    }

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

    replLogApplyOps(opCtx, cmdNss, applyOpCmd, {}, kUninitializedStmtId, {}, OplogSlot());
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
                     OplogSlot());
    }
}

namespace {

// Accepts an empty BSON builder and appends the given transaction statements to an 'applyOps' array
// field. Appends as many operations as possible until either the constructed object exceeds the
// 16MB limit or the maximum number of transaction statements allowed in one entry.
//
// If 'limitSize' is false, then it attempts to include all given operations, regardless of whether
// or not they fit. If the ops don't fit, TransactionTooLarge will be thrown in that case.
//
// Returns an iterator to the first statement that wasn't packed into the applyOps object.
std::vector<repl::ReplOperation>::const_iterator packTransactionStatementsForApplyOps(
    BSONObjBuilder* applyOpsBuilder,
    std::vector<repl::ReplOperation>::const_iterator stmtBegin,
    std::vector<repl::ReplOperation>::const_iterator stmtEnd,
    bool limitSize) {

    std::vector<repl::ReplOperation>::const_iterator stmtIter;
    BSONArrayBuilder opsArray(applyOpsBuilder->subarrayStart("applyOps"_sd));
    for (stmtIter = stmtBegin; stmtIter != stmtEnd; stmtIter++) {
        const auto& stmt = *stmtIter;
        // Stop packing when either number of transaction operations is reached, or when the next
        // one would put the array over the maximum BSON Object User Size.  We rely on the
        // head room between BSONObjMaxUserSize and BSONObjMaxInternalSize to cover the
        // BSON overhead and the other applyOps fields.  But if the array with a single operation
        // exceeds BSONObjMaxUserSize, we still log it, as a single max-length operation
        // should be able to be applied.
        if (limitSize &&
            (opsArray.arrSize() == gMaxNumberOfTransactionOperationsInSingleOplogEntry ||
             (opsArray.arrSize() > 0 &&
              (opsArray.len() + OplogEntry::getDurableReplOperationSize(stmt) >
               BSONObjMaxUserSize))))
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
// @param prepare determines whether a 'prepare' field will be attached to the written oplog entry.
// @param isPartialTxn is this applyOps entry part of an in-progress multi oplog entry transaction.
// Should be set for all non-terminal ops of an unprepared multi oplog entry transaction.
// @param shouldWriteStateField determines whether a 'state' field will be included in the write to
// the transactions table. Only meaningful if 'updateTxnTable' is true.
// @param updateTxnTable determines whether the transactions table will updated after the oplog
// entry is written.
// @param startOpTime the optime of the 'startOpTime' field of the transaction table entry update.
// If boost::none, no 'startOpTime' field will be included in the new transaction table entry. Only
// meaningful if 'updateTxnTable' is true.
//
// Returns the optime of the written oplog entry.
OpTimeBundle logApplyOpsForTransaction(OperationContext* opCtx,
                                       BSONObjBuilder* applyOpsBuilder,
                                       const OplogSlot& oplogSlot,
                                       repl::OpTime prevWriteOpTime,
                                       boost::optional<StmtId> stmtId,
                                       const bool prepare,
                                       const bool isPartialTxn,
                                       const bool shouldWriteStateField,
                                       const bool updateTxnTable,
                                       boost::optional<long long> count,
                                       boost::optional<repl::OpTime> startOpTime) {

    // A 'prepare' oplog entry should never include a 'partialTxn' field.
    invariant(!(isPartialTxn && prepare));

    const NamespaceString cmdNss{"admin", "$cmd"};

    OperationSessionInfo sessionInfo;
    repl::OplogLink oplogLink;
    sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
    sessionInfo.setTxnNumber(*opCtx->getTxnNumber());

    oplogLink.prevOpTime = prevWriteOpTime;

    try {
        if (prepare) {
            applyOpsBuilder->append("prepare", true);
        }
        if (isPartialTxn) {
            applyOpsBuilder->append("partialTxn", true);
        }
        if (count) {
            applyOpsBuilder->append("count", *count);
        }
        auto applyOpCmd = applyOpsBuilder->done();
        auto times =
            replLogApplyOps(opCtx, cmdNss, applyOpCmd, sessionInfo, stmtId, oplogLink, oplogSlot);

        auto txnState = [&]() -> boost::optional<DurableTxnStateEnum> {
            if (!shouldWriteStateField) {
                invariant(!prepare);
                return boost::none;
            }

            if (isPartialTxn) {
                return DurableTxnStateEnum::kInProgress;
            }

            return prepare ? DurableTxnStateEnum::kPrepared : DurableTxnStateEnum::kCommitted;
        }();

        if (updateTxnTable) {
            onWriteOpCompleted(
                opCtx, cmdNss, {}, times.writeOpTime, times.wallClockTime, txnState, startOpTime);
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

// Log a single applyOps for transactions without any attempt to pack operations. If the given
// statements would exceed the maximum BSON size limit of a single oplog entry, this will throw a
// TransactionTooLarge error.
// TODO (SERVER-39809): Consider removing this function once old transaction format is no longer
// needed.
OpTimeBundle logApplyOpsForTransaction(OperationContext* opCtx,
                                       const std::vector<repl::ReplOperation>& statements,
                                       const OplogSlot& oplogSlot,
                                       repl::OpTime prevWriteOpTime,
                                       boost::optional<StmtId> stmtId,
                                       const bool prepare,
                                       const bool isPartialTxn,
                                       const bool shouldWriteStateField,
                                       const bool updateTxnTable,
                                       boost::optional<long long> count,
                                       boost::optional<repl::OpTime> startOpTime) {
    BSONObjBuilder applyOpsBuilder;
    packTransactionStatementsForApplyOps(
        &applyOpsBuilder, statements.begin(), statements.end(), false /* limitSize */);
    return logApplyOpsForTransaction(opCtx,
                                     &applyOpsBuilder,
                                     oplogSlot,
                                     prevWriteOpTime,
                                     stmtId,
                                     prepare,
                                     isPartialTxn,
                                     shouldWriteStateField,
                                     updateTxnTable,
                                     count,
                                     startOpTime);
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
                                  const std::vector<repl::ReplOperation>& stmts,
                                  const std::vector<OplogSlot>& oplogSlots,
                                  bool prepare) {
    invariant(!stmts.empty());
    invariant(stmts.size() <= oplogSlots.size());

    const auto txnParticipant = TransactionParticipant::get(opCtx);
    OpTimeBundle prevWriteOpTime;
    auto numEntriesWritten = 0;
    writeConflictRetry(
        opCtx, "logOplogEntriesForTransaction", NamespaceString::kRsOplogNamespace.ns(), [&] {
            // Writes to the oplog only require a Global intent lock. Guaranteed by
            // OplogSlotReserver.
            invariant(opCtx->lockState()->isWriteLocked());

            WriteUnitOfWork wuow(opCtx);

            prevWriteOpTime.writeOpTime = txnParticipant.getLastWriteOpTime();
            auto currOplogSlot = oplogSlots.begin();

            // At the beginning of each loop iteration below, 'stmtsIter' will always point to the
            // first statement of the sequence of remaining, unpacked transaction statements. If all
            // statements have been packed, it should point to stmts.end(), which is the loop's
            // termination condition.
            auto stmtsIter = stmts.begin();
            while (stmtsIter != stmts.end()) {

                BSONObjBuilder applyOpsBuilder;
                auto nextStmt = packTransactionStatementsForApplyOps(
                    &applyOpsBuilder, stmtsIter, stmts.end(), true /* limitSize */);

                // If we packed the last op, then the next oplog entry we log should be the implicit
                // commit or implicit prepare, i.e. we omit the 'partialTxn' field.
                auto firstOp = stmtsIter == stmts.begin();
                auto lastOp = nextStmt == stmts.end();
                auto isPartialTxn = !lastOp;
                auto implicitCommit = lastOp && !prepare;
                auto implicitPrepare = lastOp && prepare;

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

                // The 'count' field gives the total number of individual operations in the
                // transaction, and is included on a non-initial implicit commit or prepare entry.
                auto count =
                    (lastOp && !firstOp) ? boost::optional<long long>(stmts.size()) : boost::none;
                prevWriteOpTime = logApplyOpsForTransaction(opCtx,
                                                            &applyOpsBuilder,
                                                            oplogSlot,
                                                            prevWriteOpTime.writeOpTime,
                                                            boost::none /* stmtId */,
                                                            implicitPrepare,
                                                            isPartialTxn,
                                                            true /* shouldWriteStateField */,
                                                            updateTxnTable,
                                                            count,
                                                            startOpTime);
                // Advance the iterator to the beginning of the remaining unpacked statements.
                stmtsIter = nextStmt;
                numEntriesWritten++;
            }
            wuow.commit();
        });
    return numEntriesWritten;
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

            // Writes to the oplog only require a Global intent lock. Guaranteed by
            // OplogSlotReserver.
            invariant(opCtx->lockState()->isWriteLocked());

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
                                                  boost::none /* stmtId */,
                                                  oplogLink,
                                                  oplogSlot);
            invariant(oplogSlot.isNull() || oplogSlot == oplogOpTime);

            onWriteOpCompleted(opCtx,
                               cmdNss,
                               {},
                               oplogOpTime,
                               wallClockTime,
                               durableState,
                               boost::none /* startOpTime */);
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

    repl::OpTime commitOpTime;
    // As FCV downgrade/upgrade is racey, we want to avoid performing a FCV check multiple times in
    // a single call into the OpObserver. Therefore, we store the result here and pass it as an
    // argument.
    const auto fcv = serverGlobalParams.featureCompatibility.getVersion();
    if (!gUseMultipleOplogEntryFormatForTransactions ||
        fcv < ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42) {
        auto txnParticipant = TransactionParticipant::get(opCtx);
        const auto lastWriteOpTime = txnParticipant.getLastWriteOpTime();
        invariant(lastWriteOpTime.isNull());
        commitOpTime = logApplyOpsForTransaction(
                           opCtx,
                           statements,
                           OplogSlot(),
                           lastWriteOpTime,
                           StmtId(0),
                           false /* prepare */,
                           false /* isPartialTxn */,
                           fcv >= ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo42,
                           true /* updateTxnTable */,
                           boost::none,
                           boost::none)
                           .writeOpTime;
    } else {
        // Reserve all the optimes in advance, so we only need to get the optime mutex once.  We
        // reserve enough entries for all statements in the transaction.
        auto oplogSlots = repl::getNextOpTimes(opCtx, statements.size());
        invariant(oplogSlots.size() == statements.size());

        // Log in-progress entries for the transaction along with the implicit commit.
        int numOplogEntries = logOplogEntriesForTransaction(opCtx, statements, oplogSlots, false);
        commitOpTime = oplogSlots[numOplogEntries - 1];
    }
    invariant(!commitOpTime.isNull());
    shardObserveTransactionPrepareOrUnpreparedCommit(opCtx, statements, commitOpTime);
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
}

void OpObserverImpl::onTransactionPrepare(OperationContext* opCtx,
                                          const std::vector<OplogSlot>& reservedSlots,
                                          std::vector<repl::ReplOperation>& statements) {
    invariant(!reservedSlots.empty());
    const auto prepareOpTime = reservedSlots.back();
    invariant(opCtx->getTxnNumber());
    invariant(!prepareOpTime.isNull());

    // Don't write oplog entry on secondaries.
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // As FCV downgrade/upgrade is racey, we want to avoid performing a FCV check multiple times in
    // a single call into the OpObserver. Therefore, we store the result here and pass it as an
    // argument.
    const auto fcv = serverGlobalParams.featureCompatibility.getVersion();
    if (!gUseMultipleOplogEntryFormatForTransactions ||
        fcv < ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42) {
        // We write the oplog entry in a side transaction so that we do not commit the now-prepared
        // transaction.
        // We write an empty 'applyOps' entry if there were no writes to choose a prepare timestamp
        // and allow this transaction to be continued on failover.
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

        writeConflictRetry(
            opCtx, "onTransactionPrepare", NamespaceString::kRsOplogNamespace.ns(), [&] {
                // Writes to the oplog only require a Global intent lock. Guaranteed by
                // OplogSlotReserver.
                invariant(opCtx->lockState()->isWriteLocked());

                WriteUnitOfWork wuow(opCtx);
                auto txnParticipant = TransactionParticipant::get(opCtx);
                const auto lastWriteOpTime = txnParticipant.getLastWriteOpTime();
                invariant(lastWriteOpTime.isNull());
                logApplyOpsForTransaction(
                    opCtx,
                    statements,
                    prepareOpTime,
                    lastWriteOpTime,
                    boost::none /* stmtId */,
                    true /* prepare */,
                    false /* isPartialTxn */,
                    fcv >= ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo42,
                    true /* updateTxnTable */,
                    boost::none /* count */,
                    prepareOpTime /* startOpTime */);
                wuow.commit();
            });
    } else {
        // We should have reserved enough slots.
        invariant(reservedSlots.size() >= statements.size());
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

        writeConflictRetry(
            opCtx, "onTransactionPrepare", NamespaceString::kRsOplogNamespace.ns(), [&] {
                // Writes to the oplog only require a Global intent lock. Guaranteed by
                // OplogSlotReserver.
                invariant(opCtx->lockState()->isWriteLocked());

                WriteUnitOfWork wuow(opCtx);
                // It is possible that the transaction resulted in no changes, In that case, we
                // should not write any operations other than the prepare oplog entry.
                if (!statements.empty()) {
                    // We had reserved enough oplog slots for the worst case where each operation
                    // produced one oplog entry.  When operations are smaller and can be packed, we
                    // will waste the extra slots.  The implicit prepare oplog entry will still use
                    // the last reserved slot, because the transaction participant has already used
                    // that as the prepare time.
                    logOplogEntriesForTransaction(
                        opCtx, statements, reservedSlots, true /* prepare */);
                } else {
                    // Log an empty 'prepare' oplog entry.
                    // We need to have at least one reserved slot.
                    invariant(reservedSlots.size() > 0);
                    BSONObjBuilder applyOpsBuilder;
                    BSONArrayBuilder opsArray(applyOpsBuilder.subarrayStart("applyOps"_sd));
                    opsArray.done();
                    auto oplogSlot = reservedSlots.front();
                    logApplyOpsForTransaction(opCtx,
                                              &applyOpsBuilder,
                                              oplogSlot,
                                              repl::OpTime() /* prevWriteOpTime */,
                                              boost::none /* stmtId */,
                                              true /* prepare */,
                                              false /* isPartialTxn */,
                                              true /* shouldWriteStateField */,
                                              true /* updateTxnTable */,
                                              boost::none /* count */,
                                              oplogSlot);
                }
                wuow.commit();
            });
    }

    shardObserveTransactionPrepareOrUnpreparedCommit(opCtx, statements, prepareOpTime);
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
}

}  // namespace mongo
