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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <memory>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/transaction_operations.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {
Date_t getDeadline(OperationContext* opCtx) {
    return opCtx->getServiceContext()->getPreciseClockSource()->now() +
        Milliseconds(resharding::gReshardingOplogApplierMaxLockRequestTimeoutMillis.load());
}

void runWithTransaction(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const boost::optional<ShardingIndexesCatalogCache>& sii,
                        unique_function<void(OperationContext*)> func) {
    AlternativeSessionRegion asr(opCtx);
    TxnNumber txnNumber = 0;
    asr.opCtx()->setTxnNumber(txnNumber);
    resharding::data_copy::runWithTransactionFromOpCtx(asr.opCtx(), nss, sii, std::move(func));
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

Status ReshardingOplogApplicationRules::applyOperation(
    OperationContext* opCtx,
    const boost::optional<ShardingIndexesCatalogCache>& sii,
    const repl::OplogEntry& op) const {
    LOGV2_DEBUG(49901, 3, "Applying op for resharding", "op"_attr = redact(op.toBSONForLogging()));

    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    invariant(opCtx->writesAreReplicated());

    return writeConflictRetry(opCtx, "applyOplogEntryCRUDOpResharding", op.getNss(), [&] {
        try {
            auto opType = op.getOpType();
            switch (opType) {
                case repl::OpTypeEnum::kInsert:
                case repl::OpTypeEnum::kUpdate:
                    _applyInsertOrUpdate(opCtx, sii, op);
                    break;
                case repl::OpTypeEnum::kDelete: {
                    _applyDelete(opCtx, sii, op);
                    _applierMetrics->onDeleteApplied();
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
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

void ReshardingOplogApplicationRules::_applyInsertOrUpdate(
    OperationContext* opCtx,
    const boost::optional<ShardingIndexesCatalogCache>& sii,
    const repl::OplogEntry& op) const {

    WriteUnitOfWork wuow(opCtx);

    auto outputColl = opCtx->runWithDeadline(getDeadline(opCtx), opCtx->getTimeoutError(), [&] {
        return acquireCollection(opCtx,
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     opCtx, _outputNss, AcquisitionPrerequisites::kWrite),
                                 MODE_IX);
    });

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to apply op during resharding due to missing collection "
                          << _outputNss.toStringForErrorMsg(),
            outputColl.exists());

    auto stashColl = opCtx->runWithDeadline(getDeadline(opCtx), opCtx->getTimeoutError(), [&] {
        return acquireCollection(opCtx,
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     opCtx, _myStashNss, AcquisitionPrerequisites::kWrite),
                                 MODE_IX);
    });

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to apply op during resharding due to missing collection "
                          << _myStashNss.toStringForErrorMsg(),
            stashColl.exists());

    auto opType = op.getOpType();
    switch (opType) {
        case repl::OpTypeEnum::kInsert:
            _applyInsert_inlock(opCtx, outputColl, stashColl, op);
            _applierMetrics->onInsertApplied();
            break;
        case repl::OpTypeEnum::kUpdate:
            _applyUpdate_inlock(opCtx, outputColl, stashColl, op);
            _applierMetrics->onUpdateApplied();
            break;
        default:
            MONGO_UNREACHABLE;
    }

    if (opCtx->recoveryUnit()->isTimestamped()) {
        // Resharding oplog application does two kinds of writes:
        //
        // 1) The (obvious) write for applying oplog entries to documents being resharded.
        // 2) A find on document in the output collection transformed into an unreplicated no-op
        // write on the same document to ensure serialization of concurrent oplog appliers reading
        // on the same doc.
        //
        // Some of the code paths can end up where only the second kind of write is made. In
        // that case, there is no timestamp associated with the write. This results in a
        // mixed-mode update chain within WT that is problematic with durable history. We
        // roll back those transactions by only committing the `WriteUnitOfWork` when there
        // is a timestamp set.
        wuow.commit();
    }
}

void ReshardingOplogApplicationRules::_applyInsert_inlock(OperationContext* opCtx,
                                                          CollectionAcquisition& outputColl,
                                                          CollectionAcquisition& stashColl,
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
    auto stashCollDoc = _queryStashCollById(opCtx, stashColl.getCollectionPtr(), idQuery);
    if (!stashCollDoc.isEmpty()) {
        auto request = UpdateRequest();
        request.setNamespaceString(_myStashNss);
        request.setQuery(idQuery);
        request.setUpdateModification(updateMod);
        request.setUpsert(false);
        request.setFromOplogApplication(true);

        UpdateResult ur = update(opCtx, stashColl, request);
        invariant(ur.numMatched != 0);

        _applierMetrics->onWriteToStashCollections();

        return;
    }

    // Query the output collection for a doc with _id == [op _id]. If a doc does not exist, apply
    // rule #2 and insert this doc into the output collection.
    BSONObj outputCollDoc;
    auto foundDoc = Helpers::findByIdAndNoopUpdate(
        opCtx, outputColl.getCollectionPtr(), idQuery, outputCollDoc);

    if (!foundDoc) {
        uassertStatusOK(Helpers::insert(opCtx, outputColl, oField));

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

        UpdateResult ur = update(opCtx, outputColl, request);
        invariant(ur.numMatched != 0);

        return;
    }

    // The doc does not belong to '_donorShardId' under the original shard key, so apply rule #4
    // and insert the contents of 'op' to the stash collection.
    uassertStatusOK(Helpers::insert(opCtx, stashColl, oField));

    _applierMetrics->onWriteToStashCollections();
}

void ReshardingOplogApplicationRules::_applyUpdate_inlock(OperationContext* opCtx,
                                                          CollectionAcquisition& outputColl,
                                                          CollectionAcquisition& stashColl,
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
    auto stashCollDoc = _queryStashCollById(opCtx, stashColl.getCollectionPtr(), idQuery);
    if (!stashCollDoc.isEmpty()) {
        auto request = UpdateRequest();
        request.setNamespaceString(_myStashNss);
        request.setQuery(idQuery);
        request.setUpdateModification(std::move(updateMod));
        request.setUpsert(false);
        request.setFromOplogApplication(true);
        UpdateResult ur = update(opCtx, stashColl, request);

        invariant(ur.numMatched != 0);

        _applierMetrics->onWriteToStashCollections();

        return;
    }

    // Query the output collection for a doc with _id == [op _id].
    BSONObj outputCollDoc;
    auto foundDoc = Helpers::findByIdAndNoopUpdate(
        opCtx, outputColl.getCollectionPtr(), idQuery, outputCollDoc);

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
    UpdateResult ur = update(opCtx, outputColl, request);

    invariant(ur.numMatched != 0);
}

void ReshardingOplogApplicationRules::_applyDelete(
    OperationContext* opCtx,
    const boost::optional<ShardingIndexesCatalogCache>& sii,
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
    {
        // First, query the conflict stash collection using [op _id] as the query. If a doc exists,
        // apply rule #1 and delete the doc from the stash collection.
        WriteUnitOfWork wuow(opCtx);

        const auto stashColl =
            opCtx->runWithDeadline(getDeadline(opCtx), opCtx->getTimeoutError(), [&] {
                return acquireCollection(opCtx,
                                         CollectionAcquisitionRequest::fromOpCtx(
                                             opCtx, _myStashNss, AcquisitionPrerequisites::kWrite),
                                         MODE_IX);
            });

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply op during resharding due to missing collection "
                              << _myStashNss.toStringForErrorMsg(),
                stashColl.exists());

        auto stashCollDoc = _queryStashCollById(opCtx, stashColl.getCollectionPtr(), idQuery);
        if (!stashCollDoc.isEmpty()) {
            auto nDeleted = deleteObjects(opCtx, stashColl, idQuery, true /* justOne */);
            invariant(nDeleted != 0);

            _applierMetrics->onWriteToStashCollections();

            invariant(opCtx->recoveryUnit()->isTimestamped());
            wuow.commit();

            return;
        }
    }

    // Now run 'findByIdAndNoopUpdate' to figure out which of rules #2, #3, and #4 we must apply.
    // We must run 'findByIdAndNoopUpdate' in the same storage transaction as the ops run in the
    // single replica set transaction that is executed if we apply rule #4, so we therefore must run
    // 'findByIdAndNoopUpdate' as a part of the single replica set transaction.
    runWithTransaction(opCtx, _outputNss, sii, [this, idQuery](OperationContext* opCtx) {
        const auto outputColl =
            opCtx->runWithDeadline(getDeadline(opCtx), opCtx->getTimeoutError(), [&] {
                return acquireCollection(
                    opCtx,
                    CollectionAcquisitionRequest::fromOpCtx(
                        opCtx, _outputNss, AcquisitionPrerequisites::OperationType::kWrite),
                    MODE_IX);
            });

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply op during resharding due to missing collection "
                              << _outputNss.toStringForErrorMsg(),
                outputColl.exists());

        // Query the output collection for a doc with _id == [op _id].
        BSONObj outputCollDoc;
        auto foundDoc = Helpers::findByIdAndNoopUpdate(
            opCtx, outputColl.getCollectionPtr(), idQuery, outputCollDoc);

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
        auto nDeleted = deleteObjects(opCtx, outputColl, idQuery, true /* justOne */);
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

            const auto stashColl =
                opCtx->runWithDeadline(getDeadline(opCtx), opCtx->getTimeoutError(), [&] {
                    return acquireCollection(
                        opCtx,
                        CollectionAcquisitionRequest::fromOpCtx(
                            opCtx, coll, AcquisitionPrerequisites::OperationType::kWrite),
                        MODE_IX);
                });

            uassert(
                ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply op during resharding due to missing collection "
                              << coll.toStringForErrorMsg(),
                stashColl.exists());

            auto request = DeleteRequest{};
            request.setNsString(coll);
            request.setQuery(idQuery);
            request.setMulti(false);
            request.setReturnDeleted(true);

            ParsedDelete parsedDelete(opCtx, &request, stashColl.getCollectionPtr());
            uassertStatusOK(parsedDelete.parseRequest());

            auto exec = uassertStatusOK(getExecutorDelete(&CurOp::get(opCtx)->debug(),
                                                          stashColl,
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
            uassertStatusOK(Helpers::insert(opCtx, outputColl, doc));
        }
    });
}

BSONObj ReshardingOplogApplicationRules::_queryStashCollById(OperationContext* opCtx,
                                                             const CollectionPtr& coll,
                                                             const BSONObj& idQuery) const {
    const IndexCatalog* indexCatalog = coll->getIndexCatalog();
    uassert(4990100,
            str::stream() << "Missing _id index for collection "
                          << _myStashNss.toStringForErrorMsg(),
            indexCatalog->haveIdIndex(opCtx));

    BSONObj result;
    Helpers::findById(opCtx, _myStashNss, idQuery, result);
    return result;
}
}  // namespace mongo
