/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_oplog_application.h"

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {
Date_t getDeadline(OperationContext* opCtx) {
    return opCtx->getServiceContext()->getPreciseClockSource()->now() +
        Milliseconds(resharding::gReshardingOplogApplierMaxLockRequestTimeoutMillis.load());
}

void runWithTransaction(OperationContext* opCtx,
                        const NamespaceString& nss,
                        unique_function<void(OperationContext*)> func) {
    AlternativeSessionRegion asr(opCtx);
    auto* const client = asr.opCtx()->getClient();
    {
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
    }
    asr.opCtx()->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    AuthorizationSession::get(client)->grantInternalAuthorization(client);
    TxnNumber txnNumber = 0;
    asr.opCtx()->setTxnNumber(txnNumber);
    asr.opCtx()->setInMultiDocumentTransaction();

    // ReshardingOpObserver depends on the collection metadata being known when processing writes to
    // the temporary resharding collection. We attach shard version IGNORED to the write operations
    // and leave it to ReshardingOplogBatchApplier::applyBatch() to retry on a StaleConfig exception
    // to allow the collection metadata information to be recovered.
    ScopedSetShardRole scopedSetShardRole(asr.opCtx(),
                                          nss,
                                          ShardVersion::IGNORED() /* shardVersion */,
                                          boost::none /* databaseVersion */);

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(asr.opCtx());
    auto ocs = mongoDSessionCatalog->checkOutSession(asr.opCtx());

    auto txnParticipant = TransactionParticipant::get(asr.opCtx());

    ScopeGuard guard([opCtx = asr.opCtx(), &txnParticipant] {
        try {
            txnParticipant.abortTransaction(opCtx);
        } catch (DBException& e) {
            LOGV2_WARNING(4990200,
                          "Failed to abort transaction in AlternativeSessionRegion",
                          "error"_attr = redact(e));
        }
    });

    txnParticipant.beginOrContinue(
        asr.opCtx(), {txnNumber}, false /* autocommit */, true /* startTransaction */);
    txnParticipant.unstashTransactionResources(asr.opCtx(), "reshardingOplogApplication");

    func(asr.opCtx());

    if (txnParticipant.retrieveCompletedTransactionOperations(asr.opCtx()).size() > 0) {
        // Similar to the `isTimestamped` check in `applyOperation`, we only want to commit the
        // transaction if we're doing replicated writes.
        txnParticipant.commitUnpreparedTransaction(asr.opCtx());
    } else {
        txnParticipant.abortTransaction(asr.opCtx());
    }
    txnParticipant.stashTransactionResources(asr.opCtx());

    guard.dismiss();
}

}  // namespace

ReshardingOplogApplicationRules::ReshardingOplogApplicationRules(
    NamespaceString outputNss,
    std::vector<NamespaceString> allStashNss,
    size_t myStashIdx,
    ShardId donorShardId,
    ChunkManager sourceChunkMgr,
    ReshardingOplogApplierMetrics* applierMetrics)
    : _outputNss(std::move(outputNss)),
      _allStashNss(std::move(allStashNss)),
      _myStashIdx(myStashIdx),
      _myStashNss(_allStashNss.at(_myStashIdx)),
      _donorShardId(std::move(donorShardId)),
      _sourceChunkMgr(std::move(sourceChunkMgr)),
      _applierMetrics(applierMetrics) {}

Status ReshardingOplogApplicationRules::applyOperation(OperationContext* opCtx,
                                                       const repl::OplogEntry& op) const {
    LOGV2_DEBUG(49901, 3, "Applying op for resharding", "op"_attr = redact(op.toBSONForLogging()));

    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    invariant(opCtx->writesAreReplicated());

    return writeConflictRetry(opCtx, "applyOplogEntryCRUDOpResharding", op.getNss().ns(), [&] {
        try {
            WriteUnitOfWork wuow(opCtx);

            AutoGetCollection autoCollOutput(opCtx,
                                             _outputNss,
                                             MODE_IX,
                                             AutoGetCollectionViewMode::kViewsForbidden,
                                             getDeadline(opCtx));
            uassert(
                ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply op during resharding due to missing collection "
                              << _outputNss.ns(),
                autoCollOutput);

            AutoGetCollection autoCollStash(opCtx,
                                            _myStashNss,
                                            MODE_IX,
                                            AutoGetCollectionViewMode::kViewsForbidden,
                                            getDeadline(opCtx));
            uassert(
                ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply op during resharding due to missing collection "
                              << _myStashNss.ns(),
                autoCollStash);

            auto opType = op.getOpType();
            switch (opType) {
                case repl::OpTypeEnum::kInsert:
                    _applyInsert_inlock(
                        opCtx, autoCollOutput.getDb(), *autoCollOutput, *autoCollStash, op);
                    _applierMetrics->onInsertApplied();

                    break;
                case repl::OpTypeEnum::kUpdate:
                    _applyUpdate_inlock(
                        opCtx, autoCollOutput.getDb(), *autoCollOutput, *autoCollStash, op);
                    _applierMetrics->onUpdateApplied();
                    break;
                case repl::OpTypeEnum::kDelete:
                    _applyDelete_inlock(
                        opCtx, autoCollOutput.getDb(), *autoCollOutput, *autoCollStash, op);
                    _applierMetrics->onDeleteApplied();
                    break;
                default:
                    MONGO_UNREACHABLE;
            }

            if (opCtx->recoveryUnit()->isTimestamped()) {
                // Resharding oplog application does two kinds of writes:
                //
                // 1) The (obvious) write for applying oplog entries to documents being resharded.
                // 2) An unreplicated no-op write that on a document in the output collection to
                //    ensure serialization of concurrent transactions.
                //
                // Some of the code paths can end up where only the second kind of write is made. In
                // that case, there is no timestamp associated with the write. This results in a
                // mixed-mode update chain within WT that is problematic with durable history. We
                // roll back those transactions by only committing the `WriteUnitOfWork` when there
                // is a timestamp set.
                wuow.commit();
            }

            return Status::OK();
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::WriteConflict) {
                throwWriteConflictException("Conflict when applying an oplog entry.");
            }

            if (ex.code() == ErrorCodes::LockTimeout) {
                throwWriteConflictException("Timeout when applying an oplog entry.");
            }

            return ex.toStatus();
        }
    });
}

void ReshardingOplogApplicationRules::_applyInsert_inlock(OperationContext* opCtx,
                                                          Database* db,
                                                          const CollectionPtr& outputColl,
                                                          const CollectionPtr& stashColl,
                                                          const repl::OplogEntry& op) const {
    /**
     * The rules to apply ordinary insert operations are as follows:
     *
     * Note that [op _id] refers to the value of op["o"]["_id"].
     *
     * 1. If there exists a document with _id == [op _id] in the conflict stash collection, replace
     * the contents of the doc in the conflict stash collection for this donor shard with the
     * contents of 'op'.
     * 2. If there does NOT exist a document with _id == [op _id] in the output collection, insert
     * the contents of 'op' into the output collection.
     * 3. If there exists a document with _id == [op _id] in the output collection and it is owned
     * by this donor shard, replace the contents of the doc in the output collection with the
     * contents of 'op'.
     * 4. If there exists a document with _id == [op _id] in the output collection and it is NOT
     * owned by this donor shard, insert the contents of 'op' into the conflict stash collection.
     */

    BSONObj oField = op.getObject();

    // If the 'o' field does not have an _id, the oplog entry is corrupted.
    auto idField = oField["_id"];
    uassert(ErrorCodes::NoSuchKey,
            str::stream() << "Failed to apply insert due to missing _id: "
                          << redact(op.toBSONForLogging()),
            !idField.eoo());

    BSONObj idQuery = idField.wrap();
    auto updateMod = write_ops::UpdateModification::parseFromClassicUpdate(oField);

    // First, query the conflict stash collection using [op _id] as the query. If a doc exists,
    // apply rule #1 and run a replacement update on the stash collection.
    auto stashCollDoc = _queryStashCollById(opCtx, stashColl, idQuery);
    if (!stashCollDoc.isEmpty()) {
        auto request = UpdateRequest();
        request.setNamespaceString(_myStashNss);
        request.setQuery(idQuery);
        request.setUpdateModification(updateMod);
        request.setUpsert(false);
        request.setFromOplogApplication(true);

        UpdateResult ur = update(opCtx, db, request);
        invariant(ur.numMatched != 0);

        _applierMetrics->onWriteToStashCollections();

        return;
    }

    // Query the output collection for a doc with _id == [op _id]. If a doc does not exist, apply
    // rule #2 and insert this doc into the output collection.
    BSONObj outputCollDoc;
    auto foundDoc = Helpers::findByIdAndNoopUpdate(opCtx, outputColl, idQuery, outputCollDoc);

    if (!foundDoc) {
        uassertStatusOK(collection_internal::insertDocument(opCtx,
                                                            outputColl,
                                                            InsertStatement(oField),
                                                            nullptr /* OpDebug */,
                                                            false /* fromMigrate */));

        return;
    }

    invariant(!outputCollDoc.isEmpty());

    // A doc with [op _id] already exists in the output collection. Check whether this doc belongs
    // to '_donorShardId' under the original shard key. If it does, apply rule #3 and run a
    // replacement update on the output collection.
    if (_sourceChunkMgr.keyBelongsToShard(
            _sourceChunkMgr.getShardKeyPattern().extractShardKeyFromDoc(outputCollDoc),
            _donorShardId)) {
        auto request = UpdateRequest();
        request.setNamespaceString(_outputNss);
        request.setQuery(idQuery);
        request.setUpdateModification(updateMod);
        request.setUpsert(false);
        request.setFromOplogApplication(true);

        UpdateResult ur = update(opCtx, db, request);
        invariant(ur.numMatched != 0);

        return;
    }

    // The doc does not belong to '_donorShardId' under the original shard key, so apply rule #4
    // and insert the contents of 'op' to the stash collection.
    uassertStatusOK(collection_internal::insertDocument(
        opCtx, stashColl, InsertStatement(oField), nullptr /* OpDebug */, false /* fromMigrate */));

    _applierMetrics->onWriteToStashCollections();
}

void ReshardingOplogApplicationRules::_applyUpdate_inlock(OperationContext* opCtx,
                                                          Database* db,
                                                          const CollectionPtr& outputColl,
                                                          const CollectionPtr& stashColl,
                                                          const repl::OplogEntry& op) const {
    /**
     * The rules to apply ordinary update operations are as follows:
     *
     * Note that [op _id] refers to the value of op["o"]["_id"].
     *
     * 1. If there exists a document with _id == [op _id] in the conflict stash collection, update
     * the document from this collection.
     * 2. If there does NOT exist a document with _id == [op _id] in the output collection, do
     * nothing.
     * 3. If there exists a document with _id == [op _id] in the output collection but it is NOT
     * owned by this donor shard, do nothing.
     * 4. If there exists a document with _id == [op _id] in the output collection and it is owned
     * by this donor shard, update the document from this collection.
     */

    BSONObj oField = op.getObject();
    BSONObj o2Field;
    if (op.getObject2())
        o2Field = op.getObject2().value();

    // If the 'o2' field does not have an _id, the oplog entry is corrupted.
    auto idField = o2Field["_id"];
    uassert(ErrorCodes::NoSuchKey,
            str::stream() << "Failed to apply update due to missing _id: "
                          << redact(op.toBSONForLogging()),
            !idField.eoo());

    BSONObj idQuery = idField.wrap();
    auto updateMod = write_ops::UpdateModification::parseFromOplogEntry(
        oField, write_ops::UpdateModification::DiffOptions{});

    // First, query the conflict stash collection using [op _id] as the query. If a doc exists,
    // apply rule #1 and update the doc from the stash collection.
    auto stashCollDoc = _queryStashCollById(opCtx, stashColl, idQuery);
    if (!stashCollDoc.isEmpty()) {
        auto request = UpdateRequest();
        request.setNamespaceString(_myStashNss);
        request.setQuery(idQuery);
        request.setUpdateModification(std::move(updateMod));
        request.setUpsert(false);
        request.setFromOplogApplication(true);
        UpdateResult ur = update(opCtx, db, request);

        invariant(ur.numMatched != 0);

        _applierMetrics->onWriteToStashCollections();

        return;
    }

    // Query the output collection for a doc with _id == [op _id].
    BSONObj outputCollDoc;
    auto foundDoc = Helpers::findByIdAndNoopUpdate(opCtx, outputColl, idQuery, outputCollDoc);

    if (!foundDoc ||
        !_sourceChunkMgr.keyBelongsToShard(
            _sourceChunkMgr.getShardKeyPattern().extractShardKeyFromDoc(outputCollDoc),
            _donorShardId)) {
        // Either a doc with _id == [op _id] does not exist in the output collection (rule
        // #2) or a doc does exist, but it does not belong to '_donorShardId' under the
        // original shard key (rule #3). In either case, do nothing.
        return;
    }

    invariant(!outputCollDoc.isEmpty());

    // A doc with _id == [op _id] exists and is owned by '_donorShardId'. Apply rule #4 and update
    // the doc in the ouput collection.
    auto request = UpdateRequest();
    request.setNamespaceString(_outputNss);
    request.setQuery(idQuery);
    request.setUpdateModification(std::move(updateMod));
    request.setUpsert(false);
    request.setFromOplogApplication(true);
    UpdateResult ur = update(opCtx, db, request);

    invariant(ur.numMatched != 0);
}

void ReshardingOplogApplicationRules::_applyDelete_inlock(OperationContext* opCtx,
                                                          Database* db,
                                                          const CollectionPtr& outputColl,
                                                          const CollectionPtr& stashColl,
                                                          const repl::OplogEntry& op) const {
    /**
     * The rules to apply ordinary delete operations are as follows:
     *
     * Note that [op _id] refers to the value of op["o"]["_id"].
     *
     * 1. If there exists a document with _id == [op _id] in the conflict stash collection, delete
     * the document from this collection.
     * 2. If there does NOT exist a document with _id == [op _id] in the output collection, do
     * nothing.
     * 3. If there exists a document with _id == [op _id] in the output collection but it is NOT
     * owned by this donor shard, do nothing.
     * 4. If there exists a document with _id == [op _id] in the output collection and it is owned
     * by this donor shard, atomically delete the doc from the output collection, choose a doc with
     * _id == [op _id] arbitrarily from among all resharding conflict stash collections to delete
     * from that resharding conflict stash collection and insert into the output collection.
     */

    BSONObj oField = op.getObject();

    // If the 'o' field does not have an _id, the oplog entry is corrupted.
    auto idField = oField["_id"];
    uassert(ErrorCodes::NoSuchKey,
            str::stream() << "Failed to apply delete due to missing _id: "
                          << redact(op.toBSONForLogging()),
            !idField.eoo());

    BSONObj idQuery = idField.wrap();
    const NamespaceString outputNss = op.getNss();

    // First, query the conflict stash collection using [op _id] as the query. If a doc exists,
    // apply rule #1 and delete the doc from the stash collection.
    auto stashCollDoc = _queryStashCollById(opCtx, stashColl, idQuery);
    if (!stashCollDoc.isEmpty()) {
        auto nDeleted = deleteObjects(opCtx, stashColl, _myStashNss, idQuery, true /* justOne */);
        invariant(nDeleted != 0);

        _applierMetrics->onWriteToStashCollections();

        return;
    }

    // Now run 'findByIdAndNoopUpdate' to figure out which of rules #2, #3, and #4 we must apply.
    // We must run 'findByIdAndNoopUpdate' in the same storage transaction as the ops run in the
    // single replica set transaction that is executed if we apply rule #4, so we therefore must run
    // 'findByIdAndNoopUpdate' as a part of the single replica set transaction.
    runWithTransaction(opCtx, _outputNss, [this, idQuery](OperationContext* opCtx) {
        AutoGetCollection autoCollOutput(opCtx,
                                         _outputNss,
                                         MODE_IX,
                                         AutoGetCollectionViewMode::kViewsForbidden,
                                         getDeadline(opCtx));
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply op during resharding due to missing collection "
                              << _outputNss.ns(),
                autoCollOutput);

        // Query the output collection for a doc with _id == [op _id].
        BSONObj outputCollDoc;
        auto foundDoc =
            Helpers::findByIdAndNoopUpdate(opCtx, *autoCollOutput, idQuery, outputCollDoc);

        if (!foundDoc ||
            !_sourceChunkMgr.keyBelongsToShard(
                _sourceChunkMgr.getShardKeyPattern().extractShardKeyFromDoc(outputCollDoc),
                _donorShardId)) {
            // Either a doc with _id == [op _id] does not exist in the output collection (rule
            // #2) or a doc does exist, but it does not belong to '_donorShardId' under the
            // original shard key (rule #3). In either case, do nothing.
            return;
        }

        invariant(!outputCollDoc.isEmpty());

        // A doc with _id == [op _id] exists and is owned by '_donorShardId'. Apply rule #4 and
        // atomically:
        // 1. Delete the doc from '_outputNss'
        // 2. Choose a document with _id == [op _id] arbitrarily from among all resharding conflict
        // stash collections to delete from that resharding conflict stash collection
        // 3. Insert the doc just deleted into the output collection

        // Delete from the output collection
        auto nDeleted =
            deleteObjects(opCtx, *autoCollOutput, _outputNss, idQuery, true /* justOne */);
        invariant(nDeleted != 0);

        // Attempt to delete a doc from one of the stash collections. Once we've matched a doc in
        // one collection, we'll break.
        BSONObj doc;
        size_t i = 0;
        for (const auto& coll : _allStashNss) {
            if (i == _myStashIdx) {
                ++i;
                continue;
            }

            AutoGetCollection autoCollStash(opCtx,
                                            coll,
                                            MODE_IX,
                                            AutoGetCollectionViewMode::kViewsForbidden,
                                            getDeadline(opCtx));
            uassert(
                ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply op during resharding due to missing collection "
                              << coll.ns(),
                autoCollStash);

            auto request = DeleteRequest{};
            request.setNsString(coll);
            request.setQuery(idQuery);
            request.setMulti(false);
            request.setReturnDeleted(true);

            ParsedDelete parsedDelete(opCtx, &request);
            uassertStatusOK(parsedDelete.parseRequest());

            auto exec = uassertStatusOK(getExecutorDelete(&CurOp::get(opCtx)->debug(),
                                                          &(*autoCollStash),
                                                          &parsedDelete,
                                                          boost::none /* verbosity */));
            BSONObj res;
            auto state = exec->getNext(&res, nullptr);

            _applierMetrics->onWriteToStashCollections();

            if (PlanExecutor::ADVANCED == state) {
                // We matched a document and deleted it, so break.
                doc = std::move(res);
                break;
            }

            invariant(state == PlanExecutor::IS_EOF);
            ++i;
        }

        // Insert the doc we just deleted from one of the stash collections into the output
        // collection.
        if (!doc.isEmpty()) {
            uassertStatusOK(collection_internal::insertDocument(opCtx,
                                                                *autoCollOutput,
                                                                InsertStatement(doc),
                                                                nullptr /* OpDebug */,
                                                                false /* fromMigrate */));
        }
    });
}

BSONObj ReshardingOplogApplicationRules::_queryStashCollById(OperationContext* opCtx,
                                                             const CollectionPtr& coll,
                                                             const BSONObj& idQuery) const {
    const IndexCatalog* indexCatalog = coll->getIndexCatalog();
    uassert(4990100,
            str::stream() << "Missing _id index for collection " << _myStashNss.ns(),
            indexCatalog->haveIdIndex(opCtx));

    BSONObj result;
    Helpers::findById(opCtx, _myStashNss.ns(), idQuery, result);
    return result;
}
}  // namespace mongo
