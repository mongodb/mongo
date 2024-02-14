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

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/metadata_consistency_types_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace metadata_consistency_util {

namespace {

MONGO_FAIL_POINT_DEFINE(insertFakeInconsistencies);

/*
 * Emit a warning log containing information about the given inconsistency
 */
void logMetadataInconsistency(const MetadataInconsistencyItem& inconsistencyItem) {
    // Please do not change the error code of this log message if not strictly necessary.
    // Automated log ingestion system relies on this specific log message to monitor cluster.
    // inconsistencies
    LOGV2_WARNING(7514800,
                  "Detected sharding metadata inconsistency",
                  "inconsistency"_attr = inconsistencyItem);
}

void _checkShardKeyIndexInconsistencies(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const ShardId& shardId,
                                        const BSONObj& shardKey,
                                        const CollectionPtr& localColl,
                                        std::vector<MetadataInconsistencyItem>& inconsistencies) {
    const auto performChecks = [&](const CollectionPtr& localColl,
                                   std::vector<MetadataInconsistencyItem>& inconsistencies) {
        // Check that the collection has an index that supports the shard key. If so, check that
        // exists an index that supports the shard key and is not multikey. We allow users to drop
        // hashed shard key indexes, and therefore we don't require hashed shard keys to have a
        // supporting index. (Ignore FCV check) Note that the feature flag ignores FCV. If this node
        // is the primary of the replica set shard, it will handle the missing hashed shard key
        // index regardless of FCV, so we skip reporting it as an inconsistency.
        const bool skipHashedShardKeyCheck =
            gFeatureFlagShardKeyIndexOptionalHashedSharding.isEnabledAndIgnoreFCVUnsafe() &&
            ShardKeyPattern(shardKey).isHashedPattern();
        if (!skipHashedShardKeyCheck &&
            !findShardKeyPrefixedIndex(opCtx, localColl, shardKey, false /*requireSingleKey*/)) {
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kMissingShardKeyIndex,
                MissingShardKeyIndexDetails{localColl->ns(), shardId, shardKey}));
        }
    };

    std::vector<MetadataInconsistencyItem> tmpInconsistencies;

    // Shards that do not own any chunks do not participate in the creation of new indexes, so they
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
                          << nss.toStringForErrorMsg(),
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

    if (!optCollDescr->hasRoutingTable()) {
        // The collection is tracked by the config server in the sharding catalog. This shard has
        // the collection locally but it is missing the routing informations
        inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
            MetadataInconsistencyTypeEnum::kShardMissingCollectionRoutingInfo,
            ShardMissingCollectionRoutingInfoDetails{localColl->ns(), localColl->uuid(), shardId}));
        return;
    }

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

std::vector<MetadataInconsistencyItem> _checkInconsistenciesBetweenBothCatalogs(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const CollectionType& catalogColl,
    const CollectionPtr& localColl) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    const auto& catalogUUID = catalogColl.getUuid();
    const auto& localUUID = localColl->uuid();
    if (catalogUUID != localUUID) {
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kCollectionUUIDMismatch,
                              CollectionUUIDMismatchDetails{nss, shardId, localUUID, catalogUUID}));
    }

    const auto makeOptionsMismatchInconsistencyBetweenShardAndConfig =
        [&](const NamespaceString& nss,
            const ShardId& shardId,
            const BSONObj& shardOptions,
            const BSONObj& configOptions) {
            constexpr StringData kShardsFieldName = "shards"_sd;
            constexpr StringData kOptionsFieldName = "options"_sd;
            const auto configShardId = Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId();

            return metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionOptionsMismatch,
                CollectionOptionsMismatchDetails{
                    nss,
                    {BSON(kOptionsFieldName << shardOptions << kShardsFieldName
                                            << BSON_ARRAY(shardId)),
                     BSON(kOptionsFieldName << configOptions << kShardsFieldName
                                            << BSON_ARRAY(configShardId))}});
        };

    // A capped collection can't be sharded.
    if (localColl->isCapped() && !catalogColl.getUnsplittable().value_or(false)) {
        inconsistencies.emplace_back(makeOptionsMismatchInconsistencyBetweenShardAndConfig(
            nss,
            shardId,
            BSON("capped" << true),
            BSON("capped" << false << CollectionType::kUnsplittableFieldName << false)));
    }

    // Verifying timeseries options are consistent between the shard and the config server.
    const auto& localTimeseriesOptions = localColl->getTimeseriesOptions();
    const auto& catalogTimeseriesOptions = [&]() -> boost::optional<TimeseriesOptions> {
        if (const auto& timeseriesFields = catalogColl.getTimeseriesFields()) {
            return timeseriesFields->getTimeseriesOptions();
        }
        return boost::none;
    }();
    if ((localTimeseriesOptions && catalogTimeseriesOptions &&
         SimpleBSONObjComparator::kInstance.evaluate(localTimeseriesOptions->toBSON() !=
                                                     catalogTimeseriesOptions->toBSON())) ||
        catalogTimeseriesOptions.has_value() != localTimeseriesOptions.has_value()) {
        inconsistencies.emplace_back(makeOptionsMismatchInconsistencyBetweenShardAndConfig(
            nss,
            shardId,
            BSON(CollectionType::kTimeseriesFieldsFieldName
                 << (localTimeseriesOptions ? localTimeseriesOptions->toBSON() : BSONObj())),
            BSON(CollectionType::kTimeseriesFieldsFieldName
                 << (catalogTimeseriesOptions ? catalogTimeseriesOptions->toBSON() : BSONObj()))));
    }

    // Verify default collation is consistent between the shard and the config server.
    if (localColl->getCollectionOptions().collation.woCompare(catalogColl.getDefaultCollation())) {
        inconsistencies.emplace_back(makeOptionsMismatchInconsistencyBetweenShardAndConfig(
            nss,
            shardId,
            BSON(CollectionType::kDefaultCollationFieldName
                 << localColl->getCollectionOptions().collation),
            BSON(CollectionType::kDefaultCollationFieldName
                 << (catalogColl.getDefaultCollation()))));
    }

    // Check shardKey index inconsistencies.
    if (catalogUUID == localUUID) {
        _checkShardKeyIndexInconsistencies(
            opCtx, nss, shardId, catalogColl.getKeyPattern().toBSON(), localColl, inconsistencies);
    }

    return inconsistencies;
}
}  // namespace


MetadataConsistencyCommandLevelEnum getCommandLevel(const NamespaceString& nss) {
    if (nss.isAdminDB()) {
        return MetadataConsistencyCommandLevelEnum::kClusterLevel;
    } else if (nss.isCollectionlessCursorNamespace()) {
        return MetadataConsistencyCommandLevelEnum::kDatabaseLevel;
    } else {
        return MetadataConsistencyCommandLevelEnum::kCollectionLevel;
    }
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeQueuedPlanExecutor(
    OperationContext* opCtx,
    std::vector<MetadataInconsistencyItem>&& inconsistencies,
    const NamespaceString& nss) {

    auto expCtx =
        make_intrusive<ExpressionContext>(opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);
    auto ws = std::make_unique<WorkingSet>();
    auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

    insertFakeInconsistencies.execute([&](const BSONObj& data) {
        const auto numInconsistencies = data["numInconsistencies"].safeNumberLong();
        for (int i = 0; i < numInconsistencies; i++) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionUUIDMismatch,
                CollectionUUIDMismatchDetails{nss, ShardId{"shard"}, UUID::gen(), UUID::gen()}));
        }
    });

    for (auto&& inconsistency : inconsistencies) {
        // Every inconsistency encountered need to be logged with the same format
        // to allow log injestion systems to correctly detect them.
        logMetadataInconsistency(inconsistency);
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
                                    PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
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

    auto&& opDebug = CurOp::get(opCtx)->debug();
    opDebug.additiveMetrics.nBatches = 1;
    opDebug.additiveMetrics.nreturned = firstBatch.size();

    if (exec->isEOF()) {
        opDebug.cursorExhausted = true;

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
    const auto cursorId = pinnedCursor.getCursor()->cursorid();
    initRespCursor.setResponseCursorBase({cursorId, nss});
    resp.setCursor(std::move(initRespCursor));

    // Record the cursorID in CurOp.
    opDebug.cursorid = cursorId;

    return resp;
}

std::vector<MetadataInconsistencyItem> checkCollectionMetadataInconsistencies(
    OperationContext* opCtx,
    const ShardId& shardId,
    const ShardId& primaryShardId,
    const std::vector<CollectionType>& shardingCatalogCollections,
    const std::vector<CollectionPtr>& localCatalogCollections) {

    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto itLocalCollections = localCatalogCollections.begin();
    auto itCatalogCollections = shardingCatalogCollections.begin();
    while (itLocalCollections != localCatalogCollections.end() &&
           itCatalogCollections != shardingCatalogCollections.end()) {
        const auto& localColl = *itLocalCollections;
        const auto& localNss = localColl->ns();
        const auto& remoteNss = itCatalogCollections->getNss();

        const auto cmp = remoteNss.coll().compare(localNss.coll());
        const bool isCollectionOnlyOnShardingCatalog = cmp < 0;
        const bool isCollectionOnBothCatalogs = cmp == 0;
        if (isCollectionOnlyOnShardingCatalog) {
            // Case where we have found a collection in the sharding catalog that it is not in the
            // local catalog.
            itCatalogCollections++;
        } else if (isCollectionOnBothCatalogs) {
            // Case where we have found same collection in the catalog client than in the local
            // catalog.
            const auto inconsistenciesBetweenBothCatalogs =
                _checkInconsistenciesBetweenBothCatalogs(
                    opCtx, localNss, shardId, *itCatalogCollections, localColl);
            inconsistencies.insert(
                inconsistencies.end(),
                std::make_move_iterator(inconsistenciesBetweenBothCatalogs.begin()),
                std::make_move_iterator(inconsistenciesBetweenBothCatalogs.end()));

            itLocalCollections++;
            itCatalogCollections++;
        } else {
            // Case where we have found a local collection that is not in the sharding catalog.
            const auto& nss = localNss;

            // TODO SERVER-59957 use function introduced in this ticket to decide if a namespace
            // should be ignored and stop using isNamepsaceAlwaysUntracked().
            if (!nss.isNamespaceAlwaysUntracked() && shardId != primaryShardId) {
                inconsistencies.emplace_back(
                    makeInconsistency(MetadataInconsistencyTypeEnum::kMisplacedCollection,
                                      MisplacedCollectionDetails{nss, shardId, localColl->uuid()}));
            }
            itLocalCollections++;
        }
    }

    if (shardId != primaryShardId) {
        // Case where we have found more local collections than in the sharding catalog. It is a
        // hidden unsharded collection inconsistency if we are not the db primary shard.
        while (itLocalCollections != localCatalogCollections.end()) {
            const auto localColl = itLocalCollections->get();
            // TODO SERVER-59957 use function introduced in this ticket to decide if a namespace
            // should be ignored and stop using isNamepsaceAlwaysUntracked().
            if (!localColl->ns().isNamespaceAlwaysUntracked()) {
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kMisplacedCollection,
                    MisplacedCollectionDetails{localColl->ns(), shardId, localColl->uuid()}));
            }
            itLocalCollections++;
        }
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

    std::vector<MetadataInconsistencyItem> inconsistencies;
    if (collection.getUnsplittable() && chunks.size() > 1) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kTrackedUnshardedCollectionHasMultipleChunks,
            TrackedUnshardedCollectionHasMultipleChunksDetails{
                nss, collection.getUuid(), int(chunks.size())}));
    }

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

std::vector<MetadataInconsistencyItem> checkZonesInconsistencies(
    OperationContext* opCtx, const CollectionType& collection, const std::vector<TagsType>& zones) {
    const auto& uuid = collection.getUuid();
    const auto& nss = collection.getNss();
    const auto shardKeyPattern = ShardKeyPattern{collection.getKeyPattern()};

    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto previousZone = zones.begin();
    for (auto it = zones.begin(); it != zones.end(); it++) {
        const auto& zone = *it;

        // Skip the first iteration as we need to compare the current zone with the previous one.
        if (it == zones.begin()) {
            continue;
        }

        if (!shardKeyPattern.isShardKey(zone.getMinKey()) ||
            !shardKeyPattern.isShardKey(zone.getMaxKey())) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kCorruptedZoneShardKey,
                CorruptedZoneShardKeyDetails{nss, uuid, zone.toBSON(), shardKeyPattern.toBSON()}));
        }

        // As the zones are sorted by minKey, we can check if the previous zone maxKey is less than
        // the current zone minKey.
        const auto& minKey = zone.getMinKey();
        auto cmp = previousZone->getMaxKey().woCompare(minKey);
        if (cmp > 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kZonesRangeOverlap,
                ZonesRangeOverlapDetails{nss, uuid, previousZone->toBSON(), zone.toBSON()}));
        }

        previousZone = it;
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkCollectionShardingMetadataConsistency(
    OperationContext* opCtx, const CollectionType& collection) {
    std::vector<MetadataInconsistencyItem> inconsistencies;
    if (collection.getUnsplittable()) {
        const auto validKey = BSON("_id" << 1);
        if (collection.getKeyPattern().toBSON().woCompare(validKey) != 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kTrackedUnshardedCollectionHasInvalidKey,
                TrackedUnshardedCollectionHasInvalidKeyDetails{
                    collection.getNss(),
                    collection.getUuid(),
                    collection.getKeyPattern().toBSON()}));
        }
    }
    return inconsistencies;
}
}  // namespace metadata_consistency_util
}  // namespace mongo
