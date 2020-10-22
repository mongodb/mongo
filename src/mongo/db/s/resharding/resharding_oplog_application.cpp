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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_oplog_application.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"

namespace mongo {

ReshardingOplogApplicationRules::ReshardingOplogApplicationRules(const NamespaceString& outputNss,
                                                                 const NamespaceString& stashNss,
                                                                 const ShardId& donorShardId,
                                                                 ChunkManager sourceChunkMgr)
    : _outputNss(outputNss),
      _stashNss(stashNss),
      _donorShardId(donorShardId),
      _sourceChunkMgr(std::move(sourceChunkMgr)) {}

Status ReshardingOplogApplicationRules::applyOperation(
    OperationContext* opCtx, const repl::OplogEntryOrGroupedInserts& opOrGroupedInserts) {
    LOGV2_DEBUG(
        49901, 3, "Applying op for resharding", "op"_attr = redact(opOrGroupedInserts.toBSON()));

    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    invariant(opCtx->writesAreReplicated());

    auto op = opOrGroupedInserts.getOp();

    return writeConflictRetry(opCtx, "applyOplogEntryResharding", op.getNss().ns(), [&] {
        Status status = Status::OK();

        WriteUnitOfWork wuow(opCtx);

        // Take the global lock now in order to avoid hitting the invariant that disallows unlocking
        // the global lock while inside a WUOW upon releasing the DB lock.
        Lock::GlobalLock globalLock(opCtx, MODE_IX);

        auto opType = op.getOpType();
        switch (opType) {
            case repl::OpTypeEnum::kInsert:
                status = _applyInsert(opCtx, opOrGroupedInserts);
                break;
            case repl::OpTypeEnum::kUpdate:
                status = _applyUpdate(opCtx, opOrGroupedInserts);
                break;
            case repl::OpTypeEnum::kDelete:
                status = _applyDelete(opCtx, opOrGroupedInserts);
                break;
            default:
                MONGO_UNREACHABLE;
        }

        if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
            throw WriteConflictException();
        }

        if (status.isOK())
            wuow.commit();

        return status;
    });
}

Status ReshardingOplogApplicationRules::_applyInsert(
    OperationContext* opCtx, const repl::OplogEntryOrGroupedInserts& opOrGroupedInserts) {
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
    auto op = opOrGroupedInserts.getOp();

    uassert(ErrorCodes::OperationFailed,
            "Cannot apply an array insert as a part of resharding oplog application",
            !opOrGroupedInserts.isGroupedInserts());

    // Writes are replicated, so use global op counters.
    OpCounters* opCounters = &globalOpCounters;
    opCounters->gotInsert();

    BSONObj o = op.getObject();

    // If the 'o' field does not have an _id, the oplog entry is corrupted.
    auto idField = o["_id"];
    uassert(ErrorCodes::NoSuchKey,
            str::stream() << "Failed to apply insert due to missing _id: " << redact(op.toBSON()),
            !idField.eoo());

    BSONObj idQuery = idField.wrap();
    const NamespaceString outputNss = op.getNss();
    auto updateMod = write_ops::UpdateModification::parseFromClassicUpdate(o);

    // First, query the conflict stash collection using [op _id] as the query. If a doc exists,
    // apply rule #1 and run a replacement update on the stash collection.
    auto stashCollDoc = _queryCollForId(opCtx, _stashNss, idQuery);
    if (!stashCollDoc.isEmpty()) {
        auto updateStashColl = [this, idQuery, updateMod](OperationContext* opCtx,
                                                          Database* db,
                                                          const AutoGetCollection& collection) {
            auto request = UpdateRequest();
            request.setNamespaceString(_stashNss);
            request.setQuery(idQuery);
            request.setUpdateModification(updateMod);
            request.setUpsert(false);
            request.setFromOplogApplication(true);

            UpdateResult ur = update(opCtx, db, request);
            invariant(ur.numMatched != 0);

            return Status::OK();
        };

        return _getCollectionAndApplyOp(opCtx, _stashNss, updateStashColl);
    }

    // Query the output collection for a doc with _id == [op _id]. If a doc does not exist, apply
    // rule #2 and insert this doc into the output collection.
    auto outputCollDoc = _queryCollForId(opCtx, _outputNss, idQuery);

    if (outputCollDoc.isEmpty()) {
        auto insertToOutputColl =
            [this, o](OperationContext* opCtx, Database* db, const AutoGetCollection& collection) {
                OpDebug* const nullOpDebug = nullptr;

                return collection->insertDocument(
                    opCtx, InsertStatement(o), nullOpDebug, false /* fromMigrate */);
            };

        return _getCollectionAndApplyOp(opCtx, _outputNss, insertToOutputColl);
    }

    // A doc with [op _id] already exists in the output collection. Check whether this doc belongs
    // to '_donorShardId' under the original shard key. If it does, apply rule #3 and run a
    // replacement update on the output collection.
    if (_sourceChunkMgr.keyBelongsToShard(
            _sourceChunkMgr.getShardKeyPattern().extractShardKeyFromDoc(outputCollDoc),
            _donorShardId)) {
        auto updateOutputCollection =
            [this, idQuery, updateMod](
                OperationContext* opCtx, Database* db, const AutoGetCollection& collection) {
                auto request = UpdateRequest();
                request.setNamespaceString(_outputNss);
                request.setQuery(idQuery);
                request.setUpdateModification(updateMod);
                request.setUpsert(false);
                request.setFromOplogApplication(true);

                UpdateResult ur = update(opCtx, db, request);
                invariant(ur.numMatched != 0);

                return Status::OK();
            };

        return _getCollectionAndApplyOp(opCtx, _outputNss, updateOutputCollection);
    }

    // The doc does not belong to '_donorShardId' under the original shard key, so apply rule #4
    // and insert the contents of 'op' to the stash collection.
    auto insertToStashColl =
        [this, o](OperationContext* opCtx, Database* db, const AutoGetCollection& collection) {
            OpDebug* const nullOpDebug = nullptr;
            return collection->insertDocument(
                opCtx, InsertStatement(o), nullOpDebug, false /* fromMigrate */);
        };

    return _getCollectionAndApplyOp(opCtx, _stashNss, insertToStashColl);
}

Status ReshardingOplogApplicationRules::_applyUpdate(
    OperationContext* opCtx, const repl::OplogEntryOrGroupedInserts& opOrGroupedInserts) {
    // TODO SERVER-49903
    return Status::OK();
}

Status ReshardingOplogApplicationRules::_applyDelete(
    OperationContext* opCtx, const repl::OplogEntryOrGroupedInserts& opOrGroupedInserts) {
    // TODO SERVER-49902
    return Status::OK();
}

Status ReshardingOplogApplicationRules::_getCollectionAndApplyOp(
    OperationContext* opCtx,
    const NamespaceString& nss,
    unique_function<Status(OperationContext*, Database*, const AutoGetCollection& collection)>
        applyOpFn) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to apply op during resharding due to missing collection "
                          << nss.ns(),
            autoColl);

    return applyOpFn(opCtx, autoColl.getDb(), autoColl);
}

BSONObj ReshardingOplogApplicationRules::_queryCollForId(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         const BSONObj& idQuery) {
    AutoGetCollectionForRead autoRead(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to apply op during resharding due to missing collection "
                          << nss.ns(),
            autoRead);

    const IndexCatalog* indexCatalog = autoRead->getIndexCatalog();
    uassert(4990100,
            str::stream() << "Missing _id index for collection " << nss.ns(),
            indexCatalog->haveIdIndex(opCtx));

    BSONObj result;
    Helpers::findById(opCtx, autoRead.getDb(), nss.ns(), idQuery, result);
    return result;
}
}  // namespace mongo
