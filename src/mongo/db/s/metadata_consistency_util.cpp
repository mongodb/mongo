/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/metadata_consistency_types_gen.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace metadata_consistency_util {

namespace {
void _checkShardKeyIndexInconsistencies(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const ShardId& shardId,
                                        const BSONObj& shardKey,
                                        const CollectionPtr& localColl,
                                        std::vector<MetadataInconsistencyItem>& inconsistencies) {
    const auto performChecks = [&](const CollectionPtr& localColl,
                                   std::vector<MetadataInconsistencyItem>& inconsistencies) {
        // Check that the collection has an index that supports the shard key. If so, check that
        // exists an index that supports the shard key and is not multikey.
        if (!findShardKeyPrefixedIndex(opCtx, localColl, shardKey, false /*requireSingleKey*/)) {
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kMissingShardKeyIndex,
                MissingShardKeyIndexDetails{localColl->ns(), shardId, shardKey}));
        }
    };

    std::vector<MetadataInconsistencyItem> tmpInconsistencies;

    // Shards that do not own any chunks do not partecipate in the creation of new indexes, so they
    // could potentially miss any indexes created after they no longer own chunks. Thus we first
    // perform a check optimistically without taking collection lock, if missing indexes are found
    // we check under the collection lock if this shard currently own any chunk and re-execute again
    // the checks under the lock to ensure stability of the ShardVersion.
    performChecks(localColl, tmpInconsistencies);

    if (!tmpInconsistencies.size()) {
        // No index inconsistencies found
        return;
    }

    // Pessimistic check under collection lock to serialize with chunk migration commit.
    AutoGetCollection ac(opCtx, nss, MODE_IS);
    tassert(7531700,
            str::stream() << "Collection unexpectedly disappeared while holding database DDL lock: "
                          << nss,
            ac);

    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
    auto optCollDescr = scopedCsr->getCurrentMetadataIfKnown();
    if (!optCollDescr) {
        LOGV2_DEBUG(7531701,
                    1,
                    "Ignoring index inconsistencies because collection metadata is unknown",
                    logAttrs(nss),
                    "inconsistencies"_attr = tmpInconsistencies);
        return;
    }

    tassert(7531702,
            str::stream()
                << "Collection unexpectedly became unsharded while holding database DDL lock: "
                << nss,
            optCollDescr->isSharded());

    if (!optCollDescr->currentShardHasAnyChunks()) {
        LOGV2_DEBUG(7531703,
                    1,
                    "Ignoring index inconsistencies because shard does not own any chunk for "
                    "this collection",
                    logAttrs(nss),
                    "inconsistencies"_attr = tmpInconsistencies);
        return;
    }

    tmpInconsistencies.clear();
    performChecks(*ac, inconsistencies);
}
}  // namespace

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeQueuedPlanExecutor(
    OperationContext* opCtx,
    const std::vector<MetadataInconsistencyItem>& inconsistencies,
    const NamespaceString& nss) {

    auto expCtx =
        make_intrusive<ExpressionContext>(opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);
    auto ws = std::make_unique<WorkingSet>();
    auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

    for (auto&& inconsistency : inconsistencies) {
        WorkingSetID id = ws->allocate();
        WorkingSetMember* member = ws->get(id);
        member->keyData.clear();
        member->recordId = RecordId();
        member->resetDocument(SnapshotId(), inconsistency.toBSON().getOwned());
        member->transitionToOwnedObj();
        root->pushBack(id);
    }

    return uassertStatusOK(
        plan_executor_factory::make(expCtx,
                                    std::move(ws),
                                    std::move(root),
                                    &CollectionPtr::null,
                                    PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                    false, /* whether returned BSON must be owned */
                                    nss));
}

CursorInitialReply createInitialCursorReplyMongod(OperationContext* opCtx,
                                                  ClientCursorParams&& cursorParams,
                                                  long long batchSize) {
    auto& exec = cursorParams.exec;
    auto& nss = cursorParams.nss;

    std::vector<BSONObj> firstBatch;
    FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
    for (long long objCount = 0; objCount < batchSize; objCount++) {
        BSONObj nextDoc;
        PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
        if (state == PlanExecutor::IS_EOF) {
            break;
        }
        invariant(state == PlanExecutor::ADVANCED);

        // If we can't fit this result inside the current batch, then we stash it for
        // later.
        if (!responseSizeTracker.haveSpaceForNext(nextDoc)) {
            exec->stashResult(nextDoc);
            break;
        }

        responseSizeTracker.add(nextDoc);
        firstBatch.push_back(std::move(nextDoc));
    }

    if (exec->isEOF()) {
        CursorInitialReply resp;
        InitialResponseCursor initRespCursor{std::move(firstBatch)};
        initRespCursor.setResponseCursorBase({0LL /* cursorId */, nss});
        resp.setCursor(std::move(initRespCursor));
        return resp;
    }

    exec->saveState();
    exec->detachFromOperationContext();

    auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(opCtx, std::move(cursorParams));

    pinnedCursor->incNBatches();
    pinnedCursor->incNReturnedSoFar(firstBatch.size());

    CursorInitialReply resp;
    InitialResponseCursor initRespCursor{std::move(firstBatch)};
    initRespCursor.setResponseCursorBase({pinnedCursor.getCursor()->cursorid(), nss});
    resp.setCursor(std::move(initRespCursor));
    return resp;
}

std::vector<MetadataInconsistencyItem> checkCollectionMetadataInconsistencies(
    OperationContext* opCtx,
    const ShardId& shardId,
    const ShardId& primaryShardId,
    const std::vector<CollectionType>& catalogClientCollections,
    const std::vector<CollectionPtr>& localCollections) {
    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto itLocalCollections = localCollections.begin();
    auto itCatalogCollections = catalogClientCollections.begin();
    while (itLocalCollections != localCollections.end() &&
           itCatalogCollections != catalogClientCollections.end()) {
        const auto& localColl = *itLocalCollections;
        const auto& localUUID = localColl->uuid();
        const auto& localNss = localColl->ns();
        const auto& nss = itCatalogCollections->getNss();

        const auto cmp = nss.coll().compare(localNss.coll());
        if (cmp < 0) {
            // Case where we have found a collection in the catalog client that it is not in the
            // local catalog.
            itCatalogCollections++;
        } else if (cmp == 0) {
            // Case where we have found same collection in the catalog client than in the local
            // catalog.

            // Check that local collection has the same UUID as the one in the catalog client.
            const auto& UUID = itCatalogCollections->getUuid();
            if (UUID != localUUID) {
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kCollectionUUIDMismatch,
                    CollectionUUIDMismatchDetails{localNss, shardId, localUUID, UUID}));
            }

            _checkShardKeyIndexInconsistencies(opCtx,
                                               nss,
                                               shardId,
                                               itCatalogCollections->getKeyPattern().toBSON(),
                                               localColl,
                                               inconsistencies);

            itLocalCollections++;
            itCatalogCollections++;
        } else {
            // Case where we have found a local collection that is not in the catalog client.
            if (shardId != primaryShardId) {
                inconsistencies.emplace_back(
                    makeInconsistency(MetadataInconsistencyTypeEnum::kMisplacedCollection,
                                      MisplacedCollectionDetails{localNss, shardId, localUUID}));
            }
            itLocalCollections++;
        }
    }

    // Case where we have found more local collections than in the catalog client. It is a
    // hidden unsharded collection inconsistency if we are not the db primary shard.
    while (itLocalCollections != localCollections.end() && shardId != primaryShardId) {
        const auto localColl = itLocalCollections->get();
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kMisplacedCollection,
            MisplacedCollectionDetails{localColl->ns(), shardId, localColl->uuid()}));
        itLocalCollections++;
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkChunksInconsistencies(
    OperationContext* opCtx,
    const CollectionType& collection,
    const std::vector<ChunkType>& chunks) {
    const auto& uuid = collection.getUuid();
    const auto& nss = collection.getNss();
    const auto shardKeyPattern = ShardKeyPattern{collection.getKeyPattern()};
    const auto configShardId = ShardId::kConfigServerId;

    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto previousChunk = chunks.begin();
    for (auto it = chunks.begin(); it != chunks.end(); it++) {
        const auto& chunk = *it;

        // Skip the first iteration as we need to compare the current chunk with the previous one.
        if (it == chunks.begin()) {
            continue;
        }

        if (!shardKeyPattern.isShardKey(chunk.getMin()) ||
            !shardKeyPattern.isShardKey(chunk.getMax())) {
            inconsistencies.emplace_back(
                makeInconsistency(MetadataInconsistencyTypeEnum::kCorruptedChunkShardKey,
                                  CorruptedChunkShardKeyDetails{
                                      nss, uuid, chunk.toConfigBSON(), shardKeyPattern.toBSON()}));
        }

        auto cmp = previousChunk->getMax().woCompare(chunk.getMin());
        if (cmp < 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableRangeGap,
                RoutingTableRangeGapDetails{
                    nss, uuid, previousChunk->toConfigBSON(), chunk.toConfigBSON()}));
        } else if (cmp > 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap,
                RoutingTableRangeOverlapDetails{
                    nss, uuid, previousChunk->toConfigBSON(), chunk.toConfigBSON()}));
        }

        previousChunk = it;
    }

    // Check if the first and last chunk have MinKey and MaxKey respectively
    if (chunks.empty()) {
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kMissingRoutingTable,
                              MissingRoutingTableDetails{nss, uuid}));
    } else {
        const BSONObj& minKeyObj = chunks.front().getMin();
        const auto globalMin = shardKeyPattern.getKeyPattern().globalMin();
        if (minKeyObj.woCompare(shardKeyPattern.getKeyPattern().globalMin()) != 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey,
                RoutingTableMissingMinKeyDetails{nss, uuid, minKeyObj, globalMin}));
        }

        const BSONObj& maxKeyObj = chunks.back().getMax();
        const auto globalMax = shardKeyPattern.getKeyPattern().globalMax();
        if (maxKeyObj.woCompare(globalMax) != 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey,
                RoutingTableMissingMaxKeyDetails{nss, uuid, maxKeyObj, globalMax}));
        }
    }

    return inconsistencies;
}

}  // namespace metadata_consistency_util
}  // namespace mongo
