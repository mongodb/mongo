// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/check_metadata_consistency_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/scoped_read_concern.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_critical_section_document_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/upgrade_downgrade_viewless_timeseries.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
using namespace std::literals::string_view_literals;
namespace metadata_consistency_util {

namespace {

MONGO_FAIL_POINT_DEFINE(insertFakeInconsistencies);
MONGO_FAIL_POINT_DEFINE(simulateCatalogTopLevelMetadataInconsistency);

static constexpr std::string_view kInMemoryShardCatalogSourceScope = "inMemoryShardCatalog"sv;
static constexpr std::string_view kDurableShardCatalogSourceScope = "durableShardCatalog"sv;
static constexpr int kConsistentSnapshotMaxRetries = 10;

/*
 * Parses a durable shard catalog document. On success returns the parsed object. If parsing fails
 * for any reason, records the inconsistency built by 'makeInconsistency' from the parse-error
 * message and returns boost::none. Reads are intentionally performed outside this helper so
 * transient read errors still propagate instead of being reported as inconsistencies.
 */
template <typename ParseFn, typename MakeInconsistencyFn>
auto parseDurableCatalogObject(const ParseFn& parse,
                               const MakeInconsistencyFn& makeInconsistency,
                               std::vector<MetadataInconsistencyItem>& inconsistencies)
    -> boost::optional<decltype(parse())> {
    try {
        return parse();
    } catch (const DBException& ex) {
        inconsistencies.emplace_back(makeInconsistency(ex.reason()));
        return boost::none;
    }
}

/*
 * This helper function returns a ScopedReadConcern with level kSnapshotReadConcern only if the
 * current readConcern doesn't have an atClusterTime, this way we keep a consistent snapshot if it
 * was provided.
 */
boost::optional<ScopedReadConcern> setSnapshotReadConcernIfNeeded(OperationContext* opCtx) {
    boost::optional<ScopedReadConcern> ret;
    if (repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime().has_value()) {
        return ret;
    }
    ret.emplace(opCtx, repl::ReadConcernArgs::kSnapshot);
    return ret;
}

/*
 * Returns the opCtx's readConcern if it is kSnapshotReadConcern, or defaultRC otherwise.
 * TODO (SERVER-131056): default to kSnapshot instead of kMajority.
 */
repl::ReadConcernArgs getReadConcernForConfigServer(
    OperationContext* opCtx, repl::ReadConcernArgs defaultRC = repl::ReadConcernArgs::kMajority) {
    repl::ReadConcernArgs ret = repl::ReadConcernArgs::get(opCtx);
    if (ret.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        return ret;
    } else {
        return defaultRC;
    }
}

MetadataInconsistencyItem makeInconsistentDurableShardCatalogMetadata(const NamespaceString& nss,
                                                                      const UUID& uuid,
                                                                      const std::string& reason) {
    return makeInconsistency(
        MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
        InconsistentShardCatalogCollectionMetadataDetails{nss, uuid, BSON("reason" << reason)});
}

/*
 * This helper throws an error for the namespace which has disappeared. The error will be a tassert
 * if it is an unexpected scenario for the collection to disappear so that our testing
 * infrastructure will catch these cases. It is a uassert with ConflictingOperationInProgress if the
 * scenario is acceptable for this to happen.
 *
 * The three accepted scenarios are:
 * 1. The current node is a secondary. Catalog stability is given by the DDL lock held by the
 *    primary, if the primary dies then there's no stability guarantee.
 * 2. The collection is `config.system.sessions` - this is acceptable since this collection is only
 *    droppable via direct shard connection.
 * 3. This node is a config server - this can happen since transitionToDedicated drops collections
 *    in the background.
 */
void throwCollectionDisappearedError(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     RSNodeMode rsMode) {
    tassert(9690601,
            str::stream() << "Collection unexpectedly disappeared while holding database DDL lock: "
                          << nss.toStringForErrorMsg(),
            rsMode != RSNodeMode::kPrimary || nss == NamespaceString::kLogicalSessionsNamespace ||
                ShardingState::get(opCtx)->shardId() == ShardId::kConfigServerId);

    uasserted(ErrorCodes::ConflictingOperationInProgress,
              str::stream() << "Collection " << nss.toStringForErrorMsg()
                            << " was dropped during CheckMetadataConsistency.");
}

/*
 * Returns the number of documents in the local collection.
 *
 * TODO SERVER-24266: get rid of the `getNumDocs` function and simply rely on `numRecords`.
 */
long long getNumDocs(OperationContext* opCtx, const Collection* localColl, RSNodeMode rsMode) {
    // Since users are advised to delete empty misplaced collections, rely on isEmpty
    // that is safe because the implementation guards against SERVER-24266.
    AutoGetCollection ac(opCtx, localColl->ns(), MODE_IS);
    if (!ac) {
        throwCollectionDisappearedError(opCtx, localColl->ns(), rsMode);
    }
    if (ac->isEmpty(opCtx)) {
        return 0;
    }
    DBDirectClient client(opCtx);
    return client.count(localColl->ns());
}

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

// TODO SERVER-108424: get rid of this check once only viewless timeseries are supported
void _checkBucketCollectionInconsistencies(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::shared_ptr<const CollectionCatalog> localCatalogSnapshot,
    const CollectionPtr& localColl,
    const bool checkView,
    RSNodeMode rsMode,
    std::vector<MetadataInconsistencyItem>& inconsistencies) {

    if (!nss.isTimeseriesBucketsCollection()) {
        return;
    }

    // This check relies on both the DDL lock being held + reads "at latest" to see the
    // timeseries view and system.buckets collection consistently and avoid false positives.
    // The DDL lock alone is not sufficient: For example, on a secondary, we may hold the DDL lock,
    // but only applied the creation of the system.buckets collection (a CreateCollectionUntracked
    // may not yet be majority committed), so we could report a false positive here.
    if (rsMode != RSNodeMode::kPrimary) {
        return;
    }

    // $out uses a temporary system.buckets collection without a view as part of its design.
    if (nss.isOutStageTmpCollection()) {
        return;
    }

    for (auto& inconsistency : timeseries::checkBucketCollectionInconsistencies(
             opCtx,
             localColl,
             checkView,
             localCatalogSnapshot->lookupView(opCtx, nss.getTimeseriesViewNamespace()).get(),
             localCatalogSnapshot->lookupCollectionByNamespace(opCtx,
                                                               nss.getTimeseriesViewNamespace()))) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kMalformedTimeseriesBucketsCollection,
            MalformedTimeseriesBucketsCollectionDetails{
                nss, std::move(inconsistency.issue), std::move(inconsistency.options)}));
    }
}

std::vector<ChunkType> getChunksFromGlobalCatalog(OperationContext* opCtx,
                                                  const CollectionType& coll,
                                                  const ShardId& shardId) {
    auto chunksStatus = Grid::get(opCtx)->catalogClient()->getChunks(
        opCtx,
        BSON(ChunkType::collectionUUID()
             << coll.getUuid() << "$or"
             << BSON_ARRAY(BSON(ChunkType::shard(shardId.toString()))
                           << BSON("history.shard" << shardId.toString()))),
        BSON(ChunkType::min() << 1),
        boost::none,
        nullptr,
        coll.getEpoch(),
        coll.getTimestamp(),
        getReadConcernForConfigServer(opCtx, repl::ReadConcernArgs::kSnapshot));

    uassertStatusOK(chunksStatus.getStatus());

    return chunksStatus.getValue();
}

std::vector<ChunkType> getChunksFromInMemoryShardCatalog(const CollectionMetadata& coll,
                                                         const ShardId& shardId) {
    std::vector<ChunkType> chunks;

    coll.getChunkManager()->forEachChunk([&](const auto& chunk) {
        bool everOwnedByShardId = (chunk.getShardId() == shardId) ||
            std::any_of(chunk.getHistory().begin(), chunk.getHistory().end(), [&](const auto& h) {
                                      return h.getShard() == shardId;
                                  });
        if (everOwnedByShardId) {
            ChunkType chunkType(
                coll.getUUID(), chunk.getRange(), chunk.getLastmod(), chunk.getShardId());
            // explicitly setting the history for history-related metadata checks
            chunkType.setHistory(chunk.getHistory());
            chunkType.setJumbo(chunk.isJumbo());
            if (!chunk.getHistory().empty()) {
                chunkType.setOnCurrentShardSince(chunk.getHistory().front().getValidAfter());
            }
            chunks.emplace_back(std::move(chunkType));
        }
        return true;
    });

    return chunks;
}

boost::optional<CollectionType> readCollectionFromDurableShardCatalog(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& uuid,
    std::vector<MetadataInconsistencyItem>& inconsistencies) {
    DBDirectClient client(opCtx);
    FindCommandRequest findOp{NamespaceString::kConfigShardCatalogCollectionsNamespace};
    findOp.setFilter(BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                              nss, SerializationContext::stateDefault())));
    auto cursor = client.find(std::move(findOp));

    tassert(10078300,
            str::stream() << "Failed to retrieve cursor while reading collection metadata for: "
                          << nss.toStringForErrorMsg(),
            cursor);

    if (!cursor->more()) {
        inconsistencies.emplace_back(makeInconsistentDurableShardCatalogMetadata(
            nss,
            uuid,
            "Collection entry not found in the durable shard catalog "
            "(config.shard.catalog.collections)"));
        return boost::none;
    }

    auto collectionDoc = cursor->nextSafe().getOwned();
    return parseDurableCatalogObject([&] { return CollectionType{collectionDoc}; },
                                     [&](const std::string& reason) {
                                         return makeInconsistentDurableShardCatalogMetadata(
                                             nss, uuid, reason);
                                     },
                                     inconsistencies);
}

bool hasChunksFromDurableShardCatalog(OperationContext* opCtx,
                                      const UUID& uuid,
                                      const ShardId& shardId) {
    const auto scopedReadConcern = setSnapshotReadConcernIfNeeded(opCtx);

    DBDirectClient client(opCtx);
    FindCommandRequest chunkFindOp{NamespaceString::kConfigShardCatalogChunksNamespace};
    chunkFindOp.setFilter(
        BSON(ChunkType::collectionUUID() << uuid << ChunkType::shard(shardId.toString())));
    chunkFindOp.setSort(BSON(ChunkType::min() << 1));
    auto chunkCursor = client.find(std::move(chunkFindOp));
    return chunkCursor->more();
}

bool hasAnyChunksFromDurableShardCatalog(OperationContext* opCtx, const UUID& uuid) {
    const auto scopedReadConcern = setSnapshotReadConcernIfNeeded(opCtx);

    DBDirectClient client(opCtx);
    FindCommandRequest chunkFindOp{NamespaceString::kConfigShardCatalogChunksNamespace};
    chunkFindOp.setFilter(BSON(ChunkType::collectionUUID() << uuid));
    chunkFindOp.setSort(BSON(ChunkType::min() << 1));
    auto chunkCursor = client.find(std::move(chunkFindOp));
    return chunkCursor->more();
}

void validateNoDurableShardCatalogEntries(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          std::vector<MetadataInconsistencyItem>& inconsistencies) {
    DBDirectClient client(opCtx);
    FindCommandRequest findOp{NamespaceString::kConfigShardCatalogCollectionsNamespace};
    findOp.setFilter(BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                              nss, SerializationContext::stateDefault())));
    auto cursor = client.find(std::move(findOp));

    tassert(12753700,
            str::stream() << "Failed to retrieve cursor while reading collection metadata for: "
                          << nss.toStringForErrorMsg(),
            cursor);

    if (cursor->more()) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                nss,
                uuid,
                BSON("reason" << "Collection entry unexpectedly found in the durable shard "
                                 "catalog (config.shard.catalog.collections)")}));
    }

    if (hasAnyChunksFromDurableShardCatalog(opCtx, uuid)) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                nss,
                uuid,
                BSON("reason" << "Chunk entry unexpectedly found in the durable shard catalog "
                                 "(config.shard.catalog.chunks)")}));
    }
}


boost::optional<std::vector<ChunkType>> readChunksFromDurableShardCatalog(
    OperationContext* opCtx,
    const CollectionType& coll,
    std::vector<MetadataInconsistencyItem>& inconsistencies) {
    const auto scopedReadConcern = setSnapshotReadConcernIfNeeded(opCtx);

    DBDirectClient client(opCtx);
    FindCommandRequest chunkFindOp{NamespaceString::kConfigShardCatalogChunksNamespace};
    chunkFindOp.setFilter(BSON(ChunkType::collectionUUID() << coll.getUuid()));
    chunkFindOp.setSort(BSON(ChunkType::min() << 1));
    auto chunkCursor = client.find(std::move(chunkFindOp));

    std::vector<ChunkType> chunks;
    while (chunkCursor->more()) {
        auto chunkDoc = chunkCursor->nextSafe().getOwned();
        auto chunk = parseDurableCatalogObject(
            [&] {
                return uassertStatusOK(
                    ChunkType::parseFromConfigBSON(chunkDoc, coll.getEpoch(), coll.getTimestamp()));
            },
            [&](const std::string& reason) {
                return makeInconsistentDurableShardCatalogMetadata(
                    coll.getNss(), coll.getUuid(), reason);
            },
            inconsistencies);
        if (!chunk) {
            return boost::none;
        }
        chunks.push_back(std::move(*chunk));
    }

    return chunks;
}

std::vector<ChunkType> filterCurrentlyOwnedChunks(const std::vector<ChunkType>& chunks,
                                                  const ShardId& shardId) {
    std::vector<ChunkType> owned;
    for (const auto& chunk : chunks) {
        if (chunk.getShard() == shardId) {
            owned.push_back(chunk);
        }
    }
    return owned;
}

boost::optional<BSONObj> validateAllShardChunksOwned(
    const std::vector<ChunkType>& shardCatalogChunks, const ShardId& shardId) {
    for (const auto& chunk : shardCatalogChunks) {
        const auto& history = chunk.getHistory();

        bool everOwnedByShardId = (chunk.getShard() == shardId) ||
            std::any_of(history.begin(), history.end(), [&](const auto& h) {
                                      return h.getShard() == shardId;
                                  });

        if (!everOwnedByShardId) {
            return BSON("reason" << "notOwnedChunkInShardCatalog"
                                 << "chunkRangeMin" << chunk.getMin() << "chunkRangeMax"
                                 << chunk.getMax() << "chunkCurrentShardId"
                                 << chunk.getShard().toString() << "shardId" << shardId.toString());
        }
    }

    return boost::none;
}

boost::optional<BSONObj> validateChunksDomainCoverage(
    const std::vector<ChunkType>& shardCatalogChunks,
    const std::vector<ChunkType>& globalCatalogChunks) {
    auto shardIt = shardCatalogChunks.begin();
    auto globalIt = globalCatalogChunks.begin();
    bool atNewRangeStart = true;

    // This loop performs a merge-walk over two sorted chunk lists (shard catalog vs global catalog)
    // to verify they cover the same key-space domain. The catalogs may split the domain into chunks
    // at different boundaries, so we track whether we're at the start of a new aligned range.

    while (shardIt != shardCatalogChunks.end() && globalIt != globalCatalogChunks.end()) {
        // At the start of each new aligned range (atNewRangeStart == true), we verify the min
        // boundaries of the current shard and global catalog chunks match.
        if (atNewRangeStart && shardIt->getMin().woCompare(globalIt->getMin()) != 0) {
            return BSON("reason" << "minBoundaryMismatch"
                                 << "shardCatalogMin" << shardIt->getMin() << "globalCatalogMin"
                                 << globalIt->getMin());
        }

        atNewRangeStart = false;

        int maxCmp = shardIt->getMax().woCompare(globalIt->getMax());

        // 1. The shard catalog chunk is a sub-range of the current global catalog chunk (shard
        // catalog is more finely split). Advance the shard iterator and verify the next shard chunk
        // is contiguous (no gap).
        if (maxCmp < 0) {
            auto prevShard = shardIt;
            shardIt++;

            if (shardIt == shardCatalogChunks.end()) {
                break;
            }

            if (shardIt->getMin().woCompare(prevShard->getMax()) != 0) {
                return BSON("reason" << "gapInShardCatalogChunks"
                                     << "prevMax" << prevShard->getMax() << "nextMin"
                                     << shardIt->getMin());
            }
        }

        // 2. Both chunks end at the same point. This closes the current aligned range. Advance both
        // iterators and mark atNewRangeStart = true so that the next iteration verifies the min
        // boundaries of the new range match.
        else if (maxCmp == 0) {
            shardIt++;
            globalIt++;
            atNewRangeStart = true;
        }

        // 3. The global catalog chunk is a sub-range of the current shard chunk (global catalog is
        // more finely split). Advance the global iterator and verify contiguity. Stay within the
        // current "range" (atNewRangeStart remains false).
        else {
            auto prevGlobal = globalIt;
            globalIt++;

            if (globalIt == globalCatalogChunks.end()) {
                break;
            }

            if (globalIt->getMin().woCompare(prevGlobal->getMax()) != 0) {
                return BSON("reason" << "gapInGlobalCatalogChunks"
                                     << "prevMax" << prevGlobal->getMax() << "nextMin"
                                     << globalIt->getMin());
            }
        }
    }

    // After the loop, any remaining unconsumed chunks in either list indicate extra coverage in one
    // catalog that the other doesn't have.

    if (shardIt != shardCatalogChunks.end()) {
        return BSON("reason" << "extraShardCatalogChunks"
                             << "chunkMin" << shardIt->getMin() << "chunkMax" << shardIt->getMax());
    }

    if (globalIt != globalCatalogChunks.end()) {
        return BSON("reason" << "extraGlobalCatalogChunks"
                             << "chunkMin" << globalIt->getMin() << "chunkMax"
                             << globalIt->getMax());
    }

    return boost::none;
}

/**
 * Chunk fields excluded from strict metadata-consistency validation.
 *
 * Every entry MUST include an inline comment explaining why it is excluded. Only add fields
 * here when global and shard catalogs are allowed to differ without indicating corruption.
 */
const stdx::unordered_set<std::string_view> kStrictChunkValidationIgnoredFields = {
    // estimatedDataSizeBytes: ephemeral balancer state written to global config.chunks during
    // defragmentation. A shard catalog commit can copy it into the durable catalog, but the shard
    // catalog never needs to read it.
    "estimatedDataSizeBytes",
    // jumbo: balancer-only hint on global config.chunks.
    // A shard catalog commit can copy it into the durable catalog, but the shard catalog never
    // needs to read it.
    "jumbo",
};

/**
 * BSON representation of per-chunk fields compared in strict metadata-consistency validation.
 * Excludes chunk _id and history (validated separately). Fields listed in
 * kStrictChunkValidationIgnoredFields are omitted here and never compared.
 */
BSONObj chunkToStrictComparableBSON(const ChunkType& chunk) {
    BSONObjBuilder builder;
    chunk.getRange().serialize(&builder);
    builder.append(ChunkType::shard.name(), chunk.getShard().toString());
    builder.appendTimestamp(ChunkType::lastmod.name(), chunk.getVersion().toLong());
    if (const auto& onCurrentShardSince = chunk.getOnCurrentShardSince()) {
        builder.append(ChunkType::onCurrentShardSince.name(), *onCurrentShardSince);
    }
    return builder.obj();
}

std::vector<std::string_view> getSortedUnionOfFieldNames(const BSONObj& lhs, const BSONObj& rhs) {
    std::set<std::string_view> fieldNames;
    for (const auto& elem : lhs) {
        fieldNames.insert(elem.fieldNameStringData());
    }
    for (const auto& elem : rhs) {
        fieldNames.insert(elem.fieldNameStringData());
    }
    return {fieldNames.begin(), fieldNames.end()};
}

bool chunkFieldValuesEqual(const BSONObj& lhs, const BSONObj& rhs, std::string_view fieldName) {
    const auto lhsElem = lhs.getField(fieldName);
    const auto rhsElem = rhs.getField(fieldName);
    if (lhsElem.eoo() && rhsElem.eoo()) {
        return true;
    }
    if (lhsElem.eoo() || rhsElem.eoo()) {
        return false;
    }
    return lhsElem.woCompare(rhsElem, /*compareFieldNames*/ false) == 0;
}

boost::optional<BSONObj> validateChunksStrictEquality(
    const std::vector<ChunkType>& shardCatalogChunks,
    const std::vector<ChunkType>& globalCatalogChunks) {
    auto shardOwned = shardCatalogChunks;
    auto globalOwned = globalCatalogChunks;
    const auto chunkRangeLess = [](const ChunkType& lhs, const ChunkType& rhs) {
        return lhs.getRange() < rhs.getRange();
    };
    std::sort(shardOwned.begin(), shardOwned.end(), chunkRangeLess);
    std::sort(globalOwned.begin(), globalOwned.end(), chunkRangeLess);

    if (shardOwned.size() != globalOwned.size()) {
        return BSON("reason" << "chunkCountMismatch"
                             << "shardCatalogCount" << static_cast<int>(shardOwned.size())
                             << "globalCatalogCount" << static_cast<int>(globalOwned.size()));
    }

    for (size_t i = 0; i < shardOwned.size(); ++i) {
        const auto& shardChunk = shardOwned[i];
        const auto& globalChunk = globalOwned[i];

        const auto shardComparable = chunkToStrictComparableBSON(shardChunk);
        const auto globalComparable = chunkToStrictComparableBSON(globalChunk);
        for (const auto& fieldName :
             getSortedUnionOfFieldNames(shardComparable, globalComparable)) {
            if (kStrictChunkValidationIgnoredFields.contains(fieldName)) {
                continue;
            }
            if (!chunkFieldValuesEqual(shardComparable, globalComparable, fieldName)) {
                const auto shardField = shardComparable.getField(fieldName);
                const auto globalField = globalComparable.getField(fieldName);
                BSONObjBuilder mismatchBuilder;
                mismatchBuilder.append("reason", "chunkFieldsMismatch");
                mismatchBuilder.append("mismatchedField", std::string(fieldName));
                mismatchBuilder.append("chunkRangeMin", shardChunk.getMin());
                if (!shardField.eoo()) {
                    mismatchBuilder.appendAs(shardField, "shardCatalogValue");
                }
                if (!globalField.eoo()) {
                    mismatchBuilder.appendAs(globalField, "globalCatalogValue");
                }
                return mismatchBuilder.obj();
            }
        }
    }

    return boost::none;
}

/**
 * Validates collection metadata on a specific shard by comparing the shard’s catalog
 * against the global catalog (either in-memory or durable).
 *
 * The following checks are performed:
 *  - Consistency of the shard catalog entry and the global catalog entry
 *  - No chunks in the shard catalog that are neither currently owned by the shard
 *    nor have ever been owned by it in the past (shard id absent from the history)
 *  - Consistency of chunk metadata between the global and shard catalogs for chunks
 *    currently owned by the shard. When the CSR is authoritative, chunks must match
 *    on all per-chunk fields except those in kStrictChunkValidationIgnoredFields (e.g.
 *    estimatedDataSizeBytes). Otherwise only domain coverage is checked, tolerating
 *    different split boundaries during legacy refresh.
 *
 * The "all shard chunks ever owned" check accepts any chunk in the shard catalog whose
 * current owner is this shard or whose history records past ownership by this shard. For
 * the durable catalog this guards against an authoritative shard catalog retaining chunks
 * the shard never owned; for the in-memory catalog it tolerates chunks that the shard owned
 * in the past in non-authoritative scenarios.
 *
 * The chunk range coverage check is restricted to chunks currently owned by the shard in
 * both catalogs, regardless of which catalog (durable or in-memory) is being validated.
 * Domain-only coverage applies when the CSR is non-authoritative. Strict per-chunk
 * validation applies when the CSR is authoritative.
 */
void validateShardCatalogEntries(ShardCatalogCollectionTypeBase shardCatalogCollection,
                                 const std::vector<ChunkType>& shardCatalogChunks,
                                 const CollectionType& globalCatalogCollection,
                                 const std::vector<ChunkType>& globalCatalogChunks,
                                 const ShardId& shardId,
                                 std::string_view sourceName,
                                 bool useStrictChunkValidation,
                                 RSNodeMode rsMode,
                                 std::vector<MetadataInconsistencyItem>& inconsistencies) {
    if (rsMode != RSNodeMode::kPrimary) {
        // The allowMigrations flag is not persisted locally, only in memory, so secondaries could
        // see an outdated value. Since this flag is not used with authoritative shards and CMC on
        // secondaries only runs with the auth shards flag enabled, this mismatch can only happen on
        // very rare occasions during setFCV, so just ignore it on secondaries.
        shardCatalogCollection.setAllowMigrations(globalCatalogCollection.getAllowMigrations()
                                                      ? boost::none
                                                      : boost::make_optional(false));
    }

    if (rsMode == RSNodeMode::kDelayedSecondary) {
        // The allowChunkOperations flag can change outside of the critical section, so delayed
        // secondaries can't check it reliably.
        shardCatalogCollection.setAllowChunkOperations(
            globalCatalogCollection.getAllowChunkOperations() ? boost::none
                                                              : boost::make_optional(false));
    }

    if (shardCatalogCollection.getComparableFields() !=
        globalCatalogCollection.getComparableFields()) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                globalCatalogCollection.getNss(),
                globalCatalogCollection.getUuid(),
                BSON("field" << "shardCatalogEntry" << "source" << sourceName << "shardCatalog"
                             << shardCatalogCollection.toBSON() << "globalCatalog"
                             << globalCatalogCollection.toShardCatalogBSON())}));
    }

    if (auto mismatchDetail = validateAllShardChunksOwned(shardCatalogChunks, shardId)) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                globalCatalogCollection.getNss(),
                globalCatalogCollection.getUuid(),
                BSON("field" << "chunkHistory"
                             << "source" << sourceName << "mismatch" << *mismatchDetail)}));
    }

    const auto shardOwnedChunks = filterCurrentlyOwnedChunks(shardCatalogChunks, shardId);
    const auto globalOwnedChunks = filterCurrentlyOwnedChunks(globalCatalogChunks, shardId);

    if (useStrictChunkValidation) {
        if (auto mismatchDetail =
                validateChunksStrictEquality(shardOwnedChunks, globalOwnedChunks)) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
                InconsistentShardCatalogCollectionMetadataDetails{
                    globalCatalogCollection.getNss(),
                    globalCatalogCollection.getUuid(),
                    BSON("field" << "chunks"
                                 << "source" << sourceName << "mismatch" << *mismatchDetail)}));
        }
    } else if (auto mismatchDetail =
                   validateChunksDomainCoverage(shardOwnedChunks, globalOwnedChunks)) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                globalCatalogCollection.getNss(),
                globalCatalogCollection.getUuid(),
                BSON("field" << "chunksDomain"
                             << "source" << sourceName << "mismatch" << *mismatchDetail)}));
    }
}

CollectionType toCollectionType(const CollectionMetadata& cm) {
    CollectionType result{cm.getChunkManager()->getNss(),
                          cm.getCollPlacementVersion().epoch(),
                          cm.getCollPlacementVersion().getTimestamp(),
                          Date_t::now(),
                          cm.getUUID(),
                          cm.getKeyPattern()};
    // Unsplittable collections only serialize the field when the flag is set to true
    if (cm.isUnsplittable()) {
        result.setUnsplittable(true);
    }
    if (auto collator = cm.getChunkManager()->getDefaultCollator()) {
        const auto& spec = collator->getSpec();
        result.setDefaultCollation(spec.toBSON());
    }
    result.setUnique(cm.isUniqueShardKey());
    result.setTimeseriesFields(cm.getTimeseriesFields());
    result.setReshardingFields(cm.getReshardingFields());
    // The allowMigrations field is only set to false in the event migrations aren't allowed. In any
    // other case it's unset.
    if (!cm.allowMigrations()) {
        result.setAllowMigrations(false);
    }
    return result;
};

bool reportIfGlobalCatalogStillOwnsChunks(const CollectionType& collectionInGlobalCatalog,
                                          const std::vector<ChunkType>& currentlyOwnedGlobalChunks,
                                          std::vector<MetadataInconsistencyItem>& inconsistencies) {
    if (currentlyOwnedGlobalChunks.empty()) {
        return false;
    }

    inconsistencies.emplace_back(makeInconsistency(
        MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
        InconsistentShardCatalogCollectionMetadataDetails{
            collectionInGlobalCatalog.getNss(),
            collectionInGlobalCatalog.getUuid(),
            BSON("field" << "ownedChunks"
                         << "source" << kInMemoryShardCatalogSourceScope
                         << "shardCatalogOwnedChunks" << 0 << "globalCatalogOwnedChunks"
                         << static_cast<int>(currentlyOwnedGlobalChunks.size()))}));
    return true;
}

void validateUnownedCsrHasNoOwnedChunks(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const ShardId& shardId,
                                        const CollectionType& collectionInGlobalCatalog,
                                        const std::vector<ChunkType>& currentlyOwnedGlobalChunks,
                                        std::vector<MetadataInconsistencyItem>& inconsistencies) {
    if (hasChunksFromDurableShardCatalog(opCtx, collectionInGlobalCatalog.getUuid(), shardId)) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                nss,
                collectionInGlobalCatalog.getUuid(),
                BSON("field" << "ownedChunks"
                             << "source" << kDurableShardCatalogSourceScope
                             << "shardCatalogHasOwnedChunks" << true << "isUnowned" << true)}));
    }

    reportIfGlobalCatalogStillOwnsChunks(
        collectionInGlobalCatalog, currentlyOwnedGlobalChunks, inconsistencies);
}

void validateInMemoryShardCatalogEntries(const CollectionMetadata& inMemoryShardCatalogMetadata,
                                         const CollectionType& collectionInGlobalCatalog,
                                         const std::vector<ChunkType>& chunksInGlobalCatalog,
                                         const ShardId& shardId,
                                         bool useStrictChunkValidation,
                                         RSNodeMode rsMode,
                                         std::vector<MetadataInconsistencyItem>& inconsistencies) {
    auto chunksInMemoryShardCatalog =
        getChunksFromInMemoryShardCatalog(inMemoryShardCatalogMetadata, shardId);

    ShardCatalogCollectionTypeBase shardCatalogCollectionInMemory =
        toCollectionType(inMemoryShardCatalogMetadata).asShardCatalogType();

    validateShardCatalogEntries(shardCatalogCollectionInMemory,
                                chunksInMemoryShardCatalog,
                                collectionInGlobalCatalog,
                                chunksInGlobalCatalog,
                                shardId,
                                kInMemoryShardCatalogSourceScope,
                                useStrictChunkValidation,
                                rsMode,
                                inconsistencies);
}

/**
 * Returns true if the in-memory shard catalog metadata is still known, no write critical section is
 * in progress, and the collection placement version still matches the snapshot taken before the
 * remote/durable reads. When false, a chunk migration may have committed in the meantime, so a
 * shard-vs-global comparison could yield false positives and must be skipped.
 */
bool isPlacementVersionStable(const CollectionShardingRuntime& csr,
                              const ChunkVersion& expectedPlacementVersion) {
    const auto currentMetadata = csr.getCurrentMetadataIfKnown();
    return currentMetadata.has_value() &&
        !csr.getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite) &&
        currentMetadata->getCollPlacementVersion() == expectedPlacementVersion;
}

/**
 * Validates an already-read durable shard catalog collection and its chunks against the global
 * catalog.
 */
void validateDurableShardCatalogEntries(const NamespaceString& nss,
                                        const ShardId& shardId,
                                        const CollectionType& collectionInGlobalCatalog,
                                        const std::vector<ChunkType>& chunksInGlobalCatalog,
                                        const CollectionType& collectionInDurableShardCatalog,
                                        const std::vector<ChunkType>& chunksInDurableShardCatalog,
                                        bool useStrictChunkValidation,
                                        RSNodeMode rsMode,
                                        std::vector<MetadataInconsistencyItem>& inconsistencies) {

    if (chunksInDurableShardCatalog.empty() && !chunksInGlobalCatalog.empty()) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                nss,
                collectionInGlobalCatalog.getUuid(),
                BSON("reason" << "Chunk entries not found in the durable shard catalog "
                                 "(config.shard.catalog.chunks)")}));
        return;
    }

    validateShardCatalogEntries(collectionInDurableShardCatalog.asShardCatalogType(),
                                chunksInDurableShardCatalog,
                                collectionInGlobalCatalog,
                                chunksInGlobalCatalog,
                                shardId,
                                kDurableShardCatalogSourceScope,
                                useStrictChunkValidation,
                                rsMode,
                                inconsistencies);
}

/**
 * Allows optimistic checks under the assumption that a FCV-gated feature flag is stable.
 * This does not prevent the feature flag from changing, it merely allows detecting the change.
 * Unlike two feature flag checks, this class detects a full FCV upgrade+downgrade (ABA problem).
 *
 * Sample usage:
 * ```cpp
 * OptimisticFCVFeatureFlagGuard flagGuard(opCtx, myFlag);
 * if (flagGuard.wasEnabled()) {
 *   auto work = doCheckAssumingMyFlagIsEnabled();
 *   if (flagGuard.validateUnchanged()) {
 *     return work;
 *   } else {
 *     return {}; // The flag was disabled during the check, so discard the result.
 *   }
 * }
 * ```
 *
 * TODO(SERVER-98118): remove once 9.0 is last LTS
 */
class OptimisticFCVFeatureFlagGuard {
public:
    explicit OptimisticFCVFeatureFlagGuard(OperationContext* opCtx,
                                           FCVGatedFeatureFlag& featureFlag)
        : _opCtx(opCtx), _featureFlag(featureFlag) {
        const auto initialFcvDoc = readFCVDocument(opCtx);
        _initialEnabled = _featureFlag.isEnabledOnVersion(initialFcvDoc.getVersion());
        _initialChangeTimestamp = initialFcvDoc.getChangeTimestamp();
    }

    bool wasEnabled() const {
        return _initialEnabled;
    }

    bool validateUnchanged() const {
        const auto currentFcvDoc = readFCVDocument(_opCtx);
        return _featureFlag.isEnabledOnVersion(currentFcvDoc.getVersion()) == _initialEnabled &&
            currentFcvDoc.getChangeTimestamp() == _initialChangeTimestamp;
    }

private:
    static FeatureCompatibilityVersionDocument readFCVDocument(OperationContext* opCtx) {
        return FeatureCompatibilityVersionDocument::parse(uassertStatusOK(
            FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(opCtx)));
    }

    OperationContext* _opCtx;
    FCVGatedFeatureFlag& _featureFlag;
    bool _initialEnabled;
    boost::optional<Timestamp> _initialChangeTimestamp;
};

void checkCollectionMetadataInShardCatalog(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    bool isPrimary,
    RSNodeMode rsMode,
    const CollectionPtr& localCollectionPtr,
    const boost::optional<CollectionType> collectionInGlobalCatalog,
    std::vector<MetadataInconsistencyItem>& inconsistencies) {
    boost::optional<CollectionMetadata> inMemoryShardCatalogMetadata;
    ChunkVersion collectionPlacementVersion;
    bool csrIsUnowned = false;

    const OptimisticFCVFeatureFlagGuard authoritativeShardsCRUD(
        opCtx, feature_flags::gAuthoritativeShardsCRUD);

    // Any inconsistency in this vector is discarded if AuthoritativeShardsCRUD gets disabled
    // TODO(SERVER-98118): simplify once 9.0 is last LTS
    std::vector<MetadataInconsistencyItem> authShardsInconsistencies;
    ON_BLOCK_EXIT([&] {
        tassert(13071400,
                "authShardsInconsistencies is not empty, but AuthShards was disabled",
                authoritativeShardsCRUD.wasEnabled() || authShardsInconsistencies.empty());
        if (!authShardsInconsistencies.empty() && !std::uncaught_exceptions() &&
            authoritativeShardsCRUD.validateUnchanged()) {
            inconsistencies.insert(inconsistencies.end(),
                                   std::make_move_iterator(authShardsInconsistencies.begin()),
                                   std::make_move_iterator(authShardsInconsistencies.end()));
        }
    });

    // Optimistic approach to avoid holding the CSR lock during the remote call to fetch information
    // about the global catalog: snapshot the shard catalog metadata and its placement version under
    // the CSR lock, release it, perform the remote reads, then re-acquire the lock and verify the
    // placement version hasn't changed.
    auto optimisticCheck = [&] {
        if (rsMode == RSNodeMode::kDelayedSecondary) {
            // The in-memory catalog metadata is not versioned, delayed secondaries can't in general
            // rely on its contents, so don't bother retrieving a CollectionMetadata but still
            // return true to make durable checks later.
            // TODO (SERVER-130947): take and keep a consistent CollectionMetadata on delayed
            // secondaries.
            return true;
        }

        const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        if (scopedCsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite)) {
            return false;
        }
        inMemoryShardCatalogMetadata = scopedCsr->getCurrentMetadataIfKnown();
        csrIsUnowned = scopedCsr->isUnowned();

        if (inMemoryShardCatalogMetadata) {
            collectionPlacementVersion = inMemoryShardCatalogMetadata->getCollPlacementVersion();
        }
        return true;
    };

    if (!optimisticCheck()) {
        return;
    }

    if (rsMode != RSNodeMode::kDelayedSecondary && !inMemoryShardCatalogMetadata &&
        TestingProctor::instance().isEnabled() && authoritativeShardsCRUD.wasEnabled() &&
        opCtx->getClient()->getPrng().trueWithProbability(
            gProbabilityOfFilteringMetadataRecovery.loadRelaxed())) {
        // Trigger a filtering metadata recovery if no data is present in memory since otherwise
        // we'd be skipping some checks.
        auto result =
            FilteringMetadataCache::get(opCtx)->onShardVersionMismatch(opCtx, nss, boost::none);
        if (!result.isOK()) {
            LOGV2_WARNING(12922400,
                          "Failed to recover collection filtering metadata from disk",
                          "error"_attr = result);
        }
        if (!optimisticCheck()) {
            return;
        }
    }

    if (!inMemoryShardCatalogMetadata) {
        // Even when in-memory metadata is unknown, authoritative shards must not have orphan
        // durable entries (for untracked collections)
        if (authoritativeShardsCRUD.wasEnabled()) {
            if (!collectionInGlobalCatalog) {
                validateNoDurableShardCatalogEntries(
                    opCtx, nss, localCollectionPtr->uuid(), authShardsInconsistencies);
            }
        }
        return;
    }

    bool expectTracked = collectionInGlobalCatalog.has_value();
    const bool csrHasRoutingTable = inMemoryShardCatalogMetadata->hasRoutingTable();

    // kUnowned is represented by metadata without a routing table, but it does not mean the
    // collection is untracked globally. It means this shard authoritatively owns no chunks and, if
    // the collection is untracked, this shard is not the DB primary. Let the dedicated unowned CSR
    // checks validate those invariants instead of reporting a generic trackedness mismatch here.

    // The corner-case when for tracked collection CSR is authoritative has no routing table.
    // This condition will be checked later.
    const bool isPrimaryWithNoRoutingTable =
        expectTracked && !csrHasRoutingTable && isPrimary && authoritativeShardsCRUD.wasEnabled();
    if (!csrIsUnowned && !isPrimaryWithNoRoutingTable &&
        ((!expectTracked && csrHasRoutingTable) || (expectTracked && !csrHasRoutingTable)) &&
        authoritativeShardsCRUD.validateUnchanged()) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                nss,
                localCollectionPtr->uuid(),
                BSON("field" << "isTracked"
                             << "isTrackedInShardCatalog" << csrHasRoutingTable
                             << "isTrackedInGlobalCatalog" << expectTracked)}));
        return;
    }

    // Unowned is a token only of the authoritative shards. Without authoritative shards, a shard
    // can keep a leftover unowned state from a previous state, but it is harmless even if used. If
    // the cluster upgrades again, the clone DDL will clean it up.
    if (authoritativeShardsCRUD.wasEnabled() && csrIsUnowned && isPrimary) {
        authShardsInconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentShardCatalogCollectionMetadata,
            InconsistentShardCatalogCollectionMetadataDetails{
                nss,
                collectionInGlobalCatalog ? collectionInGlobalCatalog->getUuid()
                                          : localCollectionPtr->uuid(),
                BSON("field" << "isPrimary"
                             << "source" << kInMemoryShardCatalogSourceScope << "isUnowned" << true
                             << "isPrimary" << true)}));
        return;
    }

    // If the collection is not tracked in the global catalog, we can stop the checks.
    if (!expectTracked) {
        if (authoritativeShardsCRUD.wasEnabled()) {
            validateNoDurableShardCatalogEntries(
                opCtx, nss, localCollectionPtr->uuid(), authShardsInconsistencies);
        }
        return;
    }

    // From this point, the collection is tracked and both shard catalog and global catalog are
    // aligned.

    // Primaries and secondaries behave slightly different from here. Primaries read the shard and
    // global catalog at potentially different timestamps, and sandwich them between calls to
    // isPlacementVersionStable() to ensure that the catalog didn't change. Secondaries can't rely
    // on that because they may be lagged compared to the CSRS and, in particular, may compare a
    // local catalog instance from before a migration commit with a global catalog from after the
    // commit. For that reason, secondaries take a PIT snapshot and make all reads at that
    // timestamp.
    // Note that we don't do the same for primaries because we want to make sure that primaries
    // always read the latest data from the config server.
    // TODO (SERVER-131440): revisit taking a PIT snapshot also on primaries.
    boost::optional<CollectionShardingRuntime::ScopedSharedCollectionShardingRuntime> scopedCsr;
    boost::optional<ScopedReadConcern> scopedReadConcern;

    auto checkCatalogStability = [&] {
        if (scopedReadConcern) {
            // With a scopedReadConcern active, we are reading at a snapshot timestamp, so there is
            // no need to check for catalog stability.
            return true;
        }
        scopedCsr.emplace(CollectionShardingRuntime::acquireShared(opCtx, nss));
        // If metadata became unknown, the critical section was acquired, or the placement version
        // changed, a migration may have occurred during the remote call. Skip the following checks
        // to avoid false positives.
        return isPlacementVersionStable(**scopedCsr, collectionPlacementVersion);
    };

    if (rsMode == RSNodeMode::kSecondary) {
        // Get the last applied timestamp and use it from now on to read from durable catalogs (both
        // the shard and the global catalog). Reacquire the CSR and check for stability. If the
        // check passes, we know that both the CSR snapshot and the timestamp are valid and
        // consistent with each other.
        const auto catalogTimestamp =
            repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime().getTimestamp();

        if (!checkCatalogStability()) {
            return;
        }

        scopedReadConcern.emplace(opCtx, [&] {
            repl::ReadConcernArgs rc = repl::ReadConcernArgs::kSnapshot;
            rc.setArgsAtClusterTimeForSnapshot(catalogTimestamp);
            return rc;
        }());
    }

    auto chunksInGlobalCatalog =
        getChunksFromGlobalCatalog(opCtx, *collectionInGlobalCatalog, shardId);

    if (!checkCatalogStability()) {
        return;
    }

    // The CSR is authoritative but has no routing table (e.g. after moveCollection away from this
    // shard). Skip in-memory validation and go directly to durable shard catalog checks.
    if (isPrimaryWithNoRoutingTable) {
        // Release the CSR lock before reading the durable shard catalog so we don't block
        // migrations/refreshes while doing storage reads.
        scopedCsr.reset();

        auto durableCollection = readCollectionFromDurableShardCatalog(
            opCtx, nss, collectionInGlobalCatalog->getUuid(), authShardsInconsistencies);
        if (!durableCollection) {
            return;
        }

        auto durableChunks =
            readChunksFromDurableShardCatalog(opCtx, *durableCollection, authShardsInconsistencies);
        if (!durableChunks) {
            return;
        }

        if (!checkCatalogStability()) {
            return;
        }
        validateDurableShardCatalogEntries(nss,
                                           shardId,
                                           *collectionInGlobalCatalog,
                                           chunksInGlobalCatalog,
                                           *durableCollection,
                                           *durableChunks,
                                           authoritativeShardsCRUD.wasEnabled(),
                                           rsMode,
                                           inconsistencies);
        return;
    }

    const auto currentlyOwnedGlobalChunks =
        filterCurrentlyOwnedChunks(chunksInGlobalCatalog, shardId);

    // If the CSR is unowned, this shard has authoritative knowledge that it owns no chunks. Since
    // unowned state is only valid on non-primary shards, and only when neither catalog says
    // this shard currently owns chunks, validate those invariants and skip the stricter metadata
    // comparison below.
    if (csrIsUnowned) {
        validateUnownedCsrHasNoOwnedChunks(opCtx,
                                           nss,
                                           shardId,
                                           *collectionInGlobalCatalog,
                                           currentlyOwnedGlobalChunks,
                                           inconsistencies);
        return;
    } else if (!inMemoryShardCatalogMetadata->getShardPlacementVersion().isSet()) {
        // The routing table exists but this shard has no owned chunks (placement version {0,0}).
        // Only check whether the global catalog still thinks we own chunks.
        reportIfGlobalCatalogStillOwnsChunks(
            *collectionInGlobalCatalog, currentlyOwnedGlobalChunks, inconsistencies);
        return;
    } else {
        bool useStrictChunkValidation =
            authoritativeShardsCRUD.wasEnabled() && authoritativeShardsCRUD.validateUnchanged();
        validateInMemoryShardCatalogEntries(*inMemoryShardCatalogMetadata,
                                            *collectionInGlobalCatalog,
                                            chunksInGlobalCatalog,
                                            shardId,
                                            useStrictChunkValidation,
                                            rsMode,
                                            inconsistencies);
    }

    if (!authoritativeShardsCRUD.wasEnabled()) {
        return;
    }

    // Release the CSR lock before reading the durable shard catalog so we don't block
    // migrations/refreshes while doing storage reads.
    scopedCsr.reset();

    auto durableCollection = readCollectionFromDurableShardCatalog(
        opCtx, nss, collectionInGlobalCatalog->getUuid(), authShardsInconsistencies);
    if (!durableCollection) {
        return;
    }

    auto durableChunks =
        readChunksFromDurableShardCatalog(opCtx, *durableCollection, authShardsInconsistencies);
    if (!durableChunks) {
        return;
    }

    if (!checkCatalogStability()) {
        return;
    }
    scopedCsr.reset();

    // Durable Shard Catalog (config.shard.catalog.*) vs Global Catalog (config.*)
    validateDurableShardCatalogEntries(nss,
                                       shardId,
                                       *collectionInGlobalCatalog,
                                       chunksInGlobalCatalog,
                                       *durableCollection,
                                       *durableChunks,
                                       authoritativeShardsCRUD.wasEnabled(),
                                       rsMode,
                                       inconsistencies);
}

void _checkShardKeyIndexInconsistencies(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const ShardId& shardId,
                                        const BSONObj& shardKey,
                                        const CollectionPtr& localColl,
                                        std::vector<MetadataInconsistencyItem>& inconsistencies,
                                        const bool checkRangeDeletionIndexes,
                                        RSNodeMode rsMode) {
    // Shards that do not own any chunks do not participate in the creation of new indexes, so they
    // could potentially miss any indexes created after they no longer own chunks. Thus we first
    // perform a check optimistically without taking collection lock, if missing indexes are found
    // we check under the collection lock if this shard currently own any chunk and re-execute again
    // the checks under the lock to ensure stability of the ShardVersion.

    // Check that the collection has an index that supports the shard key. The
    // checkMetadataConsistency function is executed under the database DDL lock, ensuring any
    // create collection operations, which run under the collection DDL lock, are serialized with
    // this check. Manual creation of a supportive shard key index (the operation does not run under
    // the DDL lock) immediately after the check below is not considered. As a result, this scenario
    // will lead to reporting an inconsistency.
    if (findShardKeyPrefixedIndex(opCtx, localColl, shardKey, false /*requireSingleKey*/)) {
        return;
    }

    std::vector<MetadataInconsistencyItem> tmpInconsistencies;

    // We allow users to drop hashed shard key indexes, and therefore we don't require hashed
    // shard keys to have a supporting index.
    if (!ShardKeyPattern(shardKey).isHashedPattern()) {
        tmpInconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
            MetadataInconsistencyTypeEnum::kMissingShardKeyIndex,
            MissingShardKeyIndexDetails{localColl->ns(), shardId, shardKey}));
    }

    if (tmpInconsistencies.size()) {
        // Pessimistic check under collection lock to serialize with chunk migration commit.
        AutoGetCollection ac(opCtx, nss, MODE_IS);
        if (!ac) {
            throwCollectionDisappearedError(opCtx, nss, rsMode);
        }

        if (findShardKeyPrefixedIndex(opCtx, *ac, shardKey, false /*requireSingleKey*/)) {
            return;
        }

        const auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
        auto optCollDescr = scopedCsr->getCurrentMetadataIfKnown();
        if (!optCollDescr) {
            LOGV2_DEBUG(7531701,
                        1,
                        "Ignoring missing shard key index because collection metadata is unknown",
                        logAttrs(nss),
                        "inconsistencies"_attr = tmpInconsistencies);
            tmpInconsistencies.clear();
        } else if (!optCollDescr->hasRoutingTable()) {
            LOGV2_DEBUG(9302300,
                        1,
                        "Ignoring missing shard key index because collection metadata is incorrect",
                        logAttrs(nss),
                        "inconsistencies"_attr = tmpInconsistencies);
            tmpInconsistencies.clear();
        } else if (!optCollDescr->currentShardHasAnyChunks()) {
            LOGV2_DEBUG(7531703,
                        1,
                        "Ignoring missing shard key index because shard does not own any chunk for "
                        "this collection",
                        logAttrs(nss),
                        "inconsistencies"_attr = tmpInconsistencies);
            tmpInconsistencies.clear();
        }
    }

    // At this point we didn't find a shard key index (even though we may not report it).

    // Copy the possible kMissingShardKeyIndex into the resulting inconsistencies.
    inconsistencies.insert(
        inconsistencies.end(), tmpInconsistencies.begin(), tmpInconsistencies.end());

    // If the checkRangeDeletionIndexes flag is set, perform an additional check to detect
    // inconsistencies in cases where a collection has an outstanding range deletion without
    // a supporting shard key index.
    if (checkRangeDeletionIndexes) {
        bool hasRangeDeletionTasks = rangedeletionutil::hasAtLeastOneRangeDeletionTaskForCollection(
            opCtx, localColl->ns(), localColl->uuid());
        if (hasRangeDeletionTasks) {
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kRangeDeletionMissingShardKeyIndex,
                RangeDeletionMissingShardKeyIndexDetails{localColl->ns(), shardId, shardKey}));
        }
    }
}

std::vector<MetadataInconsistencyItem> _checkInconsistenciesBetweenBothCatalogs(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const ShardId& primaryShardId,
    const CollectionType& catalogColl,
    const CollectionPtr& localColl,
    const bool checkRangeDeletionIndexes,
    RSNodeMode rsMode) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    const auto& catalogUUID = catalogColl.getUuid();
    const auto& localUUID = localColl->uuid();
    if (catalogUUID != localUUID) {
        // In some circumstances there might be a local old incarnation of a collection outside the
        // critical section. On delayed secondaries, return right away.
        if (rsMode == RSNodeMode::kDelayedSecondary) {
            return inconsistencies;
        }
        const auto severity =
            boost::make_optional(nss == NamespaceString::kLogicalSessionsNamespace,
                                 MetadataInconsistencySeverityEnum::kLow);
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kCollectionUUIDMismatch,
            CollectionUUIDMismatchDetails{
                nss, shardId, localUUID, catalogUUID, getNumDocs(opCtx, localColl.get(), rsMode)},
            severity));
    }

    const auto makeOptionsMismatchInconsistencyBetweenShardAndConfig =
        [&](const NamespaceString& nss,
            const ShardId& shardId,
            const BSONObj& shardOptions,
            const BSONObj& configOptions) {
            constexpr std::string_view kShardsFieldName = "shards"sv;
            constexpr std::string_view kOptionsFieldName = "options"sv;
            const auto configShardId = Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId();
            const auto severity =
                boost::make_optional(nss == NamespaceString::kLogicalSessionsNamespace,
                                     MetadataInconsistencySeverityEnum::kLow);

            return metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionOptionsMismatch,
                CollectionOptionsMismatchDetails{
                    nss,
                    {BSON(kOptionsFieldName << shardOptions << kShardsFieldName
                                            << BSON_ARRAY(shardId)),
                     BSON(kOptionsFieldName << configOptions << kShardsFieldName
                                            << BSON_ARRAY(configShardId))}},
                severity);
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
         !timeseries::optionsAreEqual(*localTimeseriesOptions, *catalogTimeseriesOptions)) ||
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

    // Check that the metadata type locally is compatible with the type of collection on the config
    // server.
    if (catalogUUID == localUUID) {
        checkCollectionMetadataInShardCatalog(opCtx,
                                              nss,
                                              shardId,
                                              shardId == primaryShardId,
                                              rsMode,
                                              localColl,
                                              catalogColl,
                                              inconsistencies);
    }

    // Check shardKey index inconsistencies.
    // Skip the check in case of unsplittable collections as we don't strictly require an index on
    // the shard key for unsplittable collections.
    // These checks rely on acquiring the CSR for the collection, so exclude delayed secondaries.
    // TODO (SERVER-130947): revisit if we can make those checks on delayed secondaries.
    const bool isSharded = !catalogColl.getUnsplittable();
    if (rsMode != RSNodeMode::kDelayedSecondary && catalogUUID == localUUID && isSharded) {
        _checkShardKeyIndexInconsistencies(opCtx,
                                           nss,
                                           shardId,
                                           catalogColl.getKeyPattern().toBSON(),
                                           localColl,
                                           inconsistencies,
                                           checkRangeDeletionIndexes,
                                           rsMode);
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> _checkLocalInconsistencies(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& currentShard,
    const ShardId& primaryShard,
    const std::shared_ptr<const CollectionCatalog> localCatalogSnapshot,
    const CollectionPtr& localColl,
    RSNodeMode rsMode) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    if (currentShard != primaryShard) {
        if (rsMode == RSNodeMode::kDelayedSecondary) {
            // movePrimary runs the clone phase outside the critical section, so there may be local
            // untracked collections while we are not the primary shard. On delayed secondaries, we
            // can't make these checks.
            return inconsistencies;
        }
        const auto numDocs = getNumDocs(opCtx, localColl.get(), rsMode);
        // config.system.sessions is created on the first data shard by CreateCollectionCoordinator,
        // not on the config server (the database primary). Ignore the transient MisplacedCollection
        // while the collection is being created and still empty.
        if (nss == NamespaceString::kLogicalSessionsNamespace && numDocs == 0) {
            LOGV2(13104700,
                  "Ignoring misplaced collection inconsistency for empty sessions collection",
                  logAttrs(nss));
        } else {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kMisplacedCollection,
                MisplacedCollectionDetails{nss, currentShard, localColl->uuid(), numDocs}));
        }
    } else {
        checkCollectionMetadataInShardCatalog(opCtx,
                                              nss,
                                              currentShard,
                                              currentShard == primaryShard,
                                              rsMode,
                                              localColl,
                                              boost::none,
                                              inconsistencies);
    }

    _checkBucketCollectionInconsistencies(opCtx,
                                          nss,
                                          localCatalogSnapshot,
                                          localColl,
                                          currentShard == primaryShard,
                                          rsMode,
                                          inconsistencies);

    return inconsistencies;
}

bool _shouldSkipOnDelayedSecondary(const NamespaceString& nss,
                                   RSNodeMode rsMode,
                                   const stdx::unordered_set<NamespaceString>& collectionsUnderCs) {
    if (rsMode != RSNodeMode::kDelayedSecondary) {
        return false;
    }

    if (collectionsUnderCs.contains(nss)) {
        LOGV2(12922301,
              "Skipping checkMetadataConsistency for collection in delayed secondary because the "
              "critical section is taken",
              "nss"_attr = nss);
        return true;
    }
    // Resharding temporary collections don't follow any normal consistency invariants. They are
    // normally hidden from CMC by the DDL lock taken by the resharding coordinator, but on delayed
    // secondaries we don't have that protection, so just ignore them altogether.
    if (nss.isTemporaryReshardingCollection()) {
        LOGV2(12922302,
              "Skipping checkMetadataConsistency for resharding temporary collection in delayed "
              "secondary",
              "nss"_attr = nss);
        return true;
    }

    return false;
}

bool _collectionMustExistLocallyButDoesnt(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const ShardId& currentShard,
                                          const ShardId& primaryShard,
                                          RSNodeMode rsMode) {
    if (rsMode == RSNodeMode::kDelayedSecondary) {
        // Delayed secondaries can't check the in-memory metadata reliably.
        // TODO (SERVER-130947): revisit this.
        return false;
    }

    // The DBPrimary shard must always have the collection created locally regardless if it owns
    // chunks or not. The config database is excluded because config.system.sessions is created on
    // the first shard instead of the database primary.
    if (currentShard == primaryShard && !nss.isConfigDB()) {
        return true;
    }

    AutoGetCollection coll(opCtx, nss, MODE_IS);
    if (coll) {
        // There is no inconsistency if the collection exists locally.
        return false;
    }

    // If the collection doesn't exist, check if the current shard owns any chunk.
    // Perform the check under the collection lock (i.e. under the AutoGetCollection scope) to make
    // sure no migration happens concurrently.
    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);

    auto optCollDescr = scopedCsr->getCurrentMetadataIfKnown();
    if (!optCollDescr) {
        LOGV2_DEBUG(7629301,
                    1,
                    "Ignoring missing collection inconsistencies because collection metadata "
                    "is unknown",
                    logAttrs(nss));
        return false;
    }
    auto criticalSectionSignal =
        scopedCsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite);
    if (criticalSectionSignal) {
        LOGV2_DEBUG(9461000,
                    1,
                    "Ignoring missing collection inconsistencies because collection metadata is "
                    "unknown when a critical section is active",
                    logAttrs(nss));
        return false;
    }

    return optCollDescr->hasRoutingTable() && optCollDescr->currentShardHasAnyChunks();
}

std::vector<BSONObj> _runExhaustiveAggregation(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               AggregateCommandRequest& aggRequest,
                                               std::string_view reason) {
    const auto logMetadataInconsistency = [](const NamespaceString& nss,
                                             const DBException& exception) {
        LOGV2(8739100,
              "Failed to refresh the routing information due to a potential metadata "
              "inconsistency",
              logAttrs(nss),
              "error"_attr = redact(exception));
    };

    std::vector<BSONObj> results;

    try {
        auto cursor = [&] {
            BSONObjBuilder responseBuilder;
            auto status = ClusterAggregate::runAggregate(opCtx,
                                                         ClusterAggregate::Namespaces{nss, nss},
                                                         aggRequest,
                                                         PrivilegeVector(),
                                                         boost::none, /*verbosity*/
                                                         &responseBuilder,
                                                         reason);
            uassertStatusOKWithContext(
                status, str::stream() << "Failed to execute aggregation for: " << reason);
            return uassertStatusOK(CursorResponse::parseFromBSON(responseBuilder.obj()));
        }();

        results = cursor.releaseBatch();

        if (!cursor.getCursorId()) {
            return results;
        }

        const auto authzSession = AuthorizationSession::get(opCtx->getClient());
        AuthzCheckFn authChecker = [&authzSession](AuthzCheckFnInputType userName) -> Status {
            return authzSession->isCoauthorizedWith(userName)
                ? Status::OK()
                : Status(ErrorCodes::Unauthorized, "User not authorized to access cursor");
        };

        // Check out the cursor. If the cursor is not found, all data was retrieve in the
        // first batch.
        const auto cursorManager = Grid::get(opCtx)->getCursorManager();
        auto pinnedCursor = uassertStatusOK(
            cursorManager->checkOutCursor(cursor.getCursorId(), opCtx, authChecker));
        while (true) {
            auto next = pinnedCursor->next();
            if (!next.isOK() || next.getValue().isEOF()) {
                break;
            }

            if (auto data = next.getValue().getResult()) {
                results.emplace_back(data.get().getOwned());
            }
        }
    } catch (const ExceptionFor<ErrorCodes::ChunkMetadataInconsistency>& e) {
        // In presence on metadata inconsistency within the config catalog, the refresh of the
        // routing information cache may fail.
        // When this happens, ignore the error: the problem will still be reported to the user
        // thanks  to the consistency checks performed on the config server.
        logMetadataInconsistency(nss, e);
    } catch (const ExceptionFor<ErrorCodes::StaleConfig>& e) {
        // ClusterAggregate has already retried stale routing errors through CollectionRouter. If
        // StaleConfig reaches this helper, the routing refresh failed to make progress.
        // Authoritative shard metadata can cause this when the shard rejects the router's
        // global-catalog view because it cannot recover matching shard-local metadata.
        logMetadataInconsistency(nss, e);
    } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& e) {
        // This is for backward compatibility reasons.
        logMetadataInconsistency(nss, e);
    }
    return results;
}

std::unique_ptr<DBClientCursor> _getCollectionChunksCursor(DBDirectClient* client,
                                                           const CollectionType& coll) {
    // Running the following pipeline against 'config.chunks':
    //    db.chunks.aggregate([{ $match: { 'uuid': <UUID> }},{ $sort: { 'min': 1 }}])
    return uassertStatusOK(DBClientCursor::fromAggregationRequest(
        client,
        std::invoke([&coll] {
            AggregateCommandRequest aggRequest{
                NamespaceString::kConfigsvrChunksNamespace,
                std::vector<mongo::BSONObj>{
                    BSON("$match" << BSON(ChunkType::collectionUUID() << coll.getUuid())),
                    BSON("$sort" << BSON(ChunkType::min() << 1))}};
            aggRequest.setReadConcern(
                repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern));
            return aggRequest;
        }),
        false /* secondaryOK */,
        false /* useExhaust */));
}

std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistencyInShardCatalogCache(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const DatabaseVersion& dbVersionInGlobalCatalog,
    const DatabaseVersion& dbVersionInShardCatalog,
    const ShardId& primaryShard) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    const auto dbVersion = [&]() {
        const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, dbName);

        // Ensure we do not access the DSS if a concurrent DDL operation (e.g., dropDatabase) is
        // holding the critical section. This race condition can occur if this command is sent by a
        // stale primary while the real primary commits changes to the catalog. Accessing the DSS
        // during a critical section violates the contract and triggers an assertion.
        scopedDsr->checkCriticalSectionOrThrow(opCtx, dbVersionInGlobalCatalog);

        return scopedDsr->getDbVersion(opCtx);
    }();

    if (!dbVersion) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalogCache,
            MissingDatabaseMetadataInShardCatalogCacheDetails{
                dbName, primaryShard, dbVersionInGlobalCatalog}));
        return inconsistencies;
    }

    const auto dbVersionInCache = *dbVersion;

    if (dbVersionInGlobalCatalog != dbVersionInCache ||
        dbVersionInShardCatalog != dbVersionInCache) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalogCache,
            InconsistentDatabaseVersionInShardCatalogCacheDetails{dbName,
                                                                  primaryShard,
                                                                  dbVersionInGlobalCatalog,
                                                                  dbVersionInShardCatalog,
                                                                  dbVersionInCache}));
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> _checkShardedCollectionUniqueIndexConsistency(
    OperationContext* opCtx,
    const CollectionPtr& localColl,
    const CollectionType& catalogColl,
    const ShardId& shardId) {
    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto& nss = localColl->ns();
    const ShardKeyPattern shardKey(catalogColl.getKeyPattern());
    auto indexCatalog = localColl->getIndexCatalog();
    auto idxIter = indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);

    while (idxIter && idxIter->more()) {
        const IndexCatalogEntry* entry = idxIter->next();
        const IndexDescriptor* descriptor = entry->descriptor();

        if (!descriptor->prepareUnique() && !descriptor->unique()) {
            continue;
        }

        auto collator = CollatorInterface::isSimpleCollator(entry->getCollator())
            ? BSONObj()
            : entry->getCollator()->getSpec().toBSON();
        auto key = descriptor->keyPattern();
        if (!shardKey.isIndexUniquenessAndCollationCompatible(key, collator)) {
            const std::string errMsg =
                "Collation must be simple and the shard key must be a prefix of the index key if "
                "the index is unique in a sharded collection";
            UniqueIndexInconsistencyInfo info{descriptor->infoObj(), shardId, errMsg};
            info.setShardKey(shardKey.toBSON());
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kIncompatibleUniqueIndexOnShardedCollection,
                InconsistentIndexDetails{nss, info.toBSON()}));
        }
    }
    return inconsistencies;
}

boost::optional<DatabaseType> readDatabaseFromDurableShardCatalog(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const DatabaseVersion& dbVersionInGlobalCatalog,
    const ShardId& primaryShard,
    std::vector<MetadataInconsistencyItem>& inconsistencies) {
    DBDirectClient client(opCtx);
    FindCommandRequest findOp{NamespaceString::kConfigShardCatalogDatabasesNamespace};
    findOp.setFilter(BSON(DatabaseType::kDbNameFieldName << DatabaseNameUtil::serialize(
                              dbName, SerializationContext::stateDefault())));
    auto cursor = client.find(std::move(findOp));

    tassert(
        10078301,
        str::stream() << "Failed to retrieve cursor while reading database metadata for database: "
                      << dbName.toStringForErrorMsg(),
        cursor);

    if (!cursor->more()) {
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog,
                              MissingDatabaseMetadataInShardCatalogDetails{
                                  dbName, primaryShard, dbVersionInGlobalCatalog}));
        return boost::none;
    }

    auto dbDoc = cursor->nextSafe().getOwned();
    auto dbInShardCatalog = parseDurableCatalogObject(
        [&] { return DatabaseType::parse(dbDoc, IDLParserContext("DatabaseType")); },
        [&](const std::string& reason) {
            MissingDatabaseMetadataInShardCatalogDetails details{
                dbName, primaryShard, dbVersionInGlobalCatalog};
            details.setReason(reason);
            return makeInconsistency(
                MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog,
                std::move(details));
        },
        inconsistencies);
    if (!dbInShardCatalog) {
        return boost::none;
    }

    tassert(9980501,
            "Found duplicated database metadata in the shard catalog with the same _id value",
            !cursor->more());

    return dbInShardCatalog;
}

std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistencyInShardCatalog(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const DatabaseVersion& dbVersionInGlobalCatalog,
    const ShardId& primaryShard,
    RSNodeMode rsMode) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    auto dbInShardCatalog = readDatabaseFromDurableShardCatalog(
        opCtx, dbName, dbVersionInGlobalCatalog, primaryShard, inconsistencies);
    if (!dbInShardCatalog) {
        return inconsistencies;
    }

    auto shardInLocalCatalog = dbInShardCatalog->getPrimary();
    if (shardInLocalCatalog != primaryShard) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalog,
            MisplacedDatabaseMetadataInShardCatalogDetails{
                dbName, primaryShard, shardInLocalCatalog}));
    }

    auto dbVersionInShardCatalog = dbInShardCatalog->getVersion();
    if (dbVersionInGlobalCatalog != dbVersionInShardCatalog) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalog,
            InconsistentDatabaseVersionInShardCatalogDetails{
                dbName, primaryShard, dbVersionInGlobalCatalog, dbVersionInShardCatalog}));
    }

    // There is currently no way to retrieve a DSR from a given timestamp, so we skip this check on
    // delayed secondaries.
    // TODO (SERVER-130947): maybe you can.
    if (rsMode != RSNodeMode::kDelayedSecondary) {
        auto cacheInconsistencies = checkDatabaseMetadataConsistencyInShardCatalogCache(
            opCtx, dbName, dbVersionInGlobalCatalog, dbVersionInShardCatalog, primaryShard);

        inconsistencies.insert(inconsistencies.end(),
                               std::make_move_iterator(cacheInconsistencies.begin()),
                               std::make_move_iterator(cacheInconsistencies.end()));
    }

    return inconsistencies;
}

std::vector<CollectionType> getCollectionsListFromConfigServer(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const MetadataConsistencyCommandLevelEnum& commandLevel) {
    switch (commandLevel) {
        case MetadataConsistencyCommandLevelEnum::kDatabaseLevel: {
            return Grid::get(opCtx)->catalogClient()->getCollections(
                opCtx,
                nss.dbName(),
                getReadConcernForConfigServer(opCtx),
                BSON(CollectionType::kNssFieldName << 1) /*sort*/);
        }
        case MetadataConsistencyCommandLevelEnum::kCollectionLevel: {
            try {
                auto collectionType = Grid::get(opCtx)->catalogClient()->getCollection(
                    opCtx, nss, getReadConcernForConfigServer(opCtx));
                return {std::move(collectionType)};
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // If we don't find the nss, it means that the collection is not sharded.
                return {};
            }
        }
        default:
            tasserted(1011704,
                      str::stream()
                          << "Unexpected parameter during the internal execution of "
                             "checkMetadataConsistency command. The shard server was expecting "
                             "to receive a database or collection level parameter, but received "
                          << idl::serialize(commandLevel) << " with namespace "
                          << nss.toStringForErrorMsg());
    }
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeQueuedPlanExecutor(
    OperationContext* opCtx,
    std::vector<MetadataInconsistencyItem>&& inconsistencies,
    const NamespaceString& nss) {

    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(nss).build();
    auto ws = std::make_unique<WorkingSet>();
    auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

    insertFakeInconsistencies.execute([&](const BSONObj& data) {
        const auto numInconsistencies = data["numInconsistencies"].safeNumberLong();
        for (int i = 0; i < numInconsistencies; i++) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionUUIDMismatch,
                CollectionUUIDMismatchDetails{nss, ShardId{"shard"}, UUID::gen(), UUID::gen(), 0}));
        }
    });

    for (auto&& inconsistency : inconsistencies) {
        // Every inconsistency encountered need to be logged with the same format
        // to allow log ingestion systems to correctly detect them.
        logMetadataInconsistency(inconsistency);
        WorkingSetID id = ws->allocate();
        WorkingSetMember* member = ws->get(id);
        member->keyData.clear();
        member->recordId = RecordId();
        member->resetDocument(SnapshotId(), inconsistency.toBSON().getOwned());
        member->transitionToOwnedObj();
        root->pushBack(id);
    }

    return plan_executor_factory::make(expCtx,
                                       std::move(ws),
                                       std::move(root),
                                       boost::none,
                                       PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                       false, /* whether returned BSON must be owned */
                                       nss);
}

auto getCatalogAndCollections(OperationContext* opCtx,
                              const NamespaceString& nss,
                              MetadataConsistencyCommandLevelEnum commandLevel) {
    std::vector<CollectionPtr> localCatalogCollections;
    switch (commandLevel) {
        case MetadataConsistencyCommandLevelEnum::kDatabaseLevel: {
            auto collCatalogSnapshot = [&] {
                // Lock db in mode IS while taking the collection catalog snapshot to ensure that we
                // serialize with non-atomic collection and index creation performed by the
                // MigrationDestinationManager. Without this lock we could potentially acquire a
                // snapshot in which a collection have been already created by the
                // MigrationDestinationManager but the relative shardkey index is still missing.
                AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IS);
                return CollectionCatalog::get(opCtx);
            }();

            for (auto&& coll : collCatalogSnapshot->range(nss.dbName())) {
                if (!coll) {
                    continue;
                }
                // The collection catalog snapshot will be in scope until the end of the command
                // execution, so we can safely use CollectionPtr_UNSAFE as the instance pointed
                // by Collection* will stay in scope as a consequence.
                localCatalogCollections.emplace_back(CollectionPtr::CollectionPtr_UNSAFE(coll));
            }
            std::sort(localCatalogCollections.begin(),
                      localCatalogCollections.end(),
                      [](const CollectionPtr& prev, const CollectionPtr& next) {
                          return prev->ns() < next->ns();
                      });

            return std::make_pair(std::move(collCatalogSnapshot),
                                  std::move(localCatalogCollections));
        }
        case MetadataConsistencyCommandLevelEnum::kCollectionLevel: {
            auto collCatalogSnapshot = [&] {
                // Lock collection in mode IS while taking the collection catalog snapshot to ensure
                // that we serialize with non-atomic collection and index creation performed by the
                // MigrationDestinationManager. Without this lock we could potentially acquire a
                // snapshot in which a collection have been already created by the
                // MigrationDestinationManager but the relative shardkey index is still missing.
                AutoGetCollection coll(opCtx,
                                       nss,
                                       MODE_IS,
                                       auto_get_collection::Options{}.viewMode(
                                           auto_get_collection::ViewMode::kViewsPermitted));
                return CollectionCatalog::get(opCtx);
            }();

            // The collection catalog snapshot will be in scope until the end of the command
            // execution, so we can safely use CollectionPtr_UNSAFE as the instance pointed by
            // Collection* will stay in scope as a consequence.
            if (auto coll = collCatalogSnapshot->lookupCollectionByNamespace(opCtx, nss)) {
                localCatalogCollections.emplace_back(CollectionPtr::CollectionPtr_UNSAFE(coll));
            }

            return std::make_pair(std::move(collCatalogSnapshot),
                                  std::move(localCatalogCollections));
        }
        default:
            tasserted(1011705,
                      str::stream()
                          << "Unexpected parameter during the internal execution of "
                             "checkMetadataConsistency command. The shard server was "
                             "expecting to receive a database or collection level parameter, but "
                             "received "
                          << idl::serialize(commandLevel) << " with namespace "
                          << nss.toStringForErrorMsg());
    }
}

auto getConsistentSnapshot(OperationContext* opCtx,
                           const NamespaceString& nss,
                           MetadataConsistencyCommandLevelEnum commandLevel) {
    using CatalogAndCollections = decltype(getCatalogAndCollections(opCtx, nss, commandLevel));
    CatalogAndCollections catalogAndCollections1;
    CatalogAndCollections catalogAndCollections2;
    Timestamp optime;
    int retries = 0;

    // Here we want to get a local catalog snapshot along with a timestamp at which the catalog was
    // valid. The idea is simple:
    //  1. Take the latest catalog snapshot.
    //  2. Get the last applied timestamp.
    //  3. Take the latest catalog again.
    // If the catalog snapshots taken at 1 and 3 are equal, that means that none of the oplog
    // entries that were applied between the two acquisitions changed the catalog, and in particular
    // the one timestamp we took in the middle is consistent with the catalog snapshot.
    do {
        retries++;
        if (retries >= kConsistentSnapshotMaxRetries) {
            optime = Timestamp{};
            break;
        }

        catalogAndCollections1 = getCatalogAndCollections(opCtx, nss, commandLevel);
        optime = repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedTimestamp();
        catalogAndCollections2 = getCatalogAndCollections(opCtx, nss, commandLevel);
    } while (!optime.isNull() &&
             std::get<0>(catalogAndCollections1) != std::get<0>(catalogAndCollections2));

    if (optime.isNull()) {
        static constexpr char msg[] =
            "Couldn't get a consistent snapshot/timestamp pair for check metadata consistency on a "
            "delayed secondary";
        LOGV2_WARNING(
            12922305, msg, "nss"_attr = nss, "commandLevel"_attr = idl::serialize(commandLevel));
        uasserted(ErrorCodes::SnapshotUnavailable, msg);
    }

    return std::tuple_cat(std::move(catalogAndCollections2),
                          std::tuple{boost::make_optional(optime)});
}

stdx::unordered_set<NamespaceString> getCollectionsUnderCriticalSection(
    OperationContext* opCtx, const std::vector<CollectionPtr>& localCatalogCollections) {
    DBDirectClient dbClient(opCtx);
    FindCommandRequest request{NamespaceString::kCollectionCriticalSectionsNamespace};
    request.setFilter(BSON(CollectionCriticalSectionDocument::kBlockReadsFieldName << true));
    auto cursor = dbClient.find(std::move(request));
    stdx::unordered_set<NamespaceString> namespacesUnderCS;

    while (cursor->more()) {
        const auto obj = cursor->next();
        const auto nssName =
            obj[CollectionCriticalSectionDocument::kNssFieldName].valueStringDataSafe();
        if (nssName.empty()) {
            continue;
        }
        auto nss = NamespaceStringUtil::deserialize(
            boost::none, nssName, SerializationContext::stateDefault());
        uassert(
            ErrorCodes::SnapshotUnavailable,
            fmt::format("The database critical section is taken: {}", nss.toStringForErrorMsg()),
            !nss.isDbOnly());
        namespacesUnderCS.emplace(std::move(nss));
    }

    return namespacesUnderCS;
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

CursorInitialReply createInitialCursorReplyMongod(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::vector<MetadataInconsistencyItem>&& inconsistencies,
    const boost::optional<mongo::SimpleCursorOptions>& requestCursorOpts,
    const BSONObj& request,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor) {

    const auto batchSize = [&]() -> int64_t {
        if (requestCursorOpts && requestCursorOpts->getBatchSize()) {
            return *requestCursorOpts->getBatchSize();
        } else {
            return query_request_helper::getDefaultBatchSize();
        }
    }();

    if (!planExecutor) {
        planExecutor = makeQueuedPlanExecutor(opCtx, std::move(inconsistencies), nss);
    } else {
        // A streaming executor (e.g. one merging remote cursors) was supplied. Emit the
        // locally-computed inconsistencies first by stashing them at the front of the executor's
        // output, then stream the executor's own results.
        for (auto&& inconsistency : inconsistencies) {
            logMetadataInconsistency(inconsistency);
            planExecutor->stashResult(inconsistency.toBSON());
        }
    }

    auto* const exec = planExecutor.get();

    ClientCursorParams cursorParams{
        std::move(planExecutor),
        nss,
        AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
        APIParameters::get(opCtx),
        opCtx->getWriteConcern(),
        repl::ReadConcernArgs::get(opCtx),
        ReadPreferenceSetting::get(opCtx),
        request,
        {Privilege(ResourcePattern::forClusterResource(nss.tenantId()), ActionType::internal)}};

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
    opDebug.getAdditiveMetrics().nBatches = 1;
    opDebug.getAdditiveMetrics().nreturned = firstBatch.size();

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

std::vector<MetadataInconsistencyItem> checkCollectionMetadataConsistency(
    OperationContext* opCtx,
    const ShardId& shardId,
    const ShardId& primaryShardId,
    const std::vector<CollectionType>& shardingCatalogCollections,
    const std::shared_ptr<const CollectionCatalog> localCatalogSnapshot,
    const std::vector<CollectionPtr>& localCatalogCollections,
    const bool checkRangeDeletionIndexes,
    const bool optionalCheckIndexes,
    RSNodeMode rsMode,
    const stdx::unordered_set<NamespaceString>& collectionsUnderCs) {

    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto itLocalCollections = localCatalogCollections.begin();
    auto itCatalogCollections = shardingCatalogCollections.begin();
    while (itLocalCollections != localCatalogCollections.end() &&
           itCatalogCollections != shardingCatalogCollections.end()) {
        const auto& catalogColl = *itCatalogCollections;
        const auto& localColl = *itLocalCollections;
        const auto& localNss = localColl->ns();
        const auto& remoteNss = catalogColl.getNss();

        const auto cmp = remoteNss.coll().compare(localNss.coll());
        const bool isCollectionOnlyOnShardingCatalog = cmp < 0;
        const bool isCollectionOnBothCatalogs = cmp == 0;
        if (isCollectionOnlyOnShardingCatalog) {
            // Ignore the edge-case with logical sessions collection persisted despite at times
            // missing from the local collection. This can happen only on a node that also is a
            // config.
            if (remoteNss == NamespaceString::kLogicalSessionsNamespace &&
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                itCatalogCollections++;
                continue;
            }
            // Case where we have found a collection in the sharding catalog that it is not in the
            // local catalog.
            if (_collectionMustExistLocallyButDoesnt(
                    opCtx, remoteNss, shardId, primaryShardId, rsMode)) {
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kMissingLocalCollection,
                    MissingLocalCollectionDetails{remoteNss, catalogColl.getUuid(), shardId}));
            }
            itCatalogCollections++;
        } else if (isCollectionOnBothCatalogs) {
            if (_shouldSkipOnDelayedSecondary(remoteNss, rsMode, collectionsUnderCs)) {
                itLocalCollections++;
                itCatalogCollections++;
                continue;
            }

            // Case where we have found same collection in the catalog client than in the local
            // catalog.
            auto inconsistenciesBetweenBothCatalogs =
                _checkInconsistenciesBetweenBothCatalogs(opCtx,
                                                         localNss,
                                                         shardId,
                                                         primaryShardId,
                                                         catalogColl,
                                                         localColl,
                                                         checkRangeDeletionIndexes,
                                                         rsMode);
            inconsistencies.insert(
                inconsistencies.end(),
                std::make_move_iterator(inconsistenciesBetweenBothCatalogs.begin()),
                std::make_move_iterator(inconsistenciesBetweenBothCatalogs.end()));

            itLocalCollections++;
            itCatalogCollections++;

            _checkBucketCollectionInconsistencies(opCtx,
                                                  localNss,
                                                  localCatalogSnapshot,
                                                  localColl,
                                                  primaryShardId == shardId /* isPrimaryShard */,
                                                  rsMode,
                                                  inconsistencies);
            if (optionalCheckIndexes && !catalogColl.getUnsplittable()) {
                auto indexesInconsistencies = _checkShardedCollectionUniqueIndexConsistency(
                    opCtx, localColl, catalogColl, shardId);

                inconsistencies.insert(inconsistencies.end(),
                                       std::make_move_iterator(indexesInconsistencies.begin()),
                                       std::make_move_iterator(indexesInconsistencies.end()));
            }
        } else {
            // Case where we have found a local collection that is not in the sharding catalog.
            const auto& nss = localNss;

            if (!localNss.isShardLocalNamespace() &&
                !_shouldSkipOnDelayedSecondary(localNss, rsMode, collectionsUnderCs)) {
                auto localInconsistencies = _checkLocalInconsistencies(
                    opCtx, nss, shardId, primaryShardId, localCatalogSnapshot, localColl, rsMode);
                inconsistencies.insert(inconsistencies.end(),
                                       std::make_move_iterator(localInconsistencies.begin()),
                                       std::make_move_iterator(localInconsistencies.end()));
            }
            itLocalCollections++;
        }
    }

    while (itLocalCollections != localCatalogCollections.end()) {
        const auto& localColl = *itLocalCollections;
        const auto& localNss = localColl->ns();

        if (!localNss.isShardLocalNamespace() &&
            !_shouldSkipOnDelayedSecondary(localNss, rsMode, collectionsUnderCs)) {
            auto localInconsistencies = _checkLocalInconsistencies(
                opCtx, localNss, shardId, primaryShardId, localCatalogSnapshot, localColl, rsMode);
            inconsistencies.insert(inconsistencies.end(),
                                   std::make_move_iterator(localInconsistencies.begin()),
                                   std::make_move_iterator(localInconsistencies.end()));
        }
        itLocalCollections++;
    }

    while (itCatalogCollections != shardingCatalogCollections.end()) {
        if (_collectionMustExistLocallyButDoesnt(
                opCtx, itCatalogCollections->getNss(), shardId, primaryShardId, rsMode)) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kMissingLocalCollection,
                MissingLocalCollectionDetails{
                    itCatalogCollections->getNss(), itCatalogCollections->getUuid(), shardId}));
        }
        itCatalogCollections++;
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkIndexesConsistencyAcrossShards(
    OperationContext* opCtx, const std::vector<CollectionType>& collections) {
    static const auto rawPipelineStages = [] {
        /**
         * The following pipeline is used to check for inconsistencies in the indexes of all the
         * collections across all shards in the cluster. In particular, it checks that:
         *      1. All shards have the same set of indexes.
         *      2. All shards have the same properties for each index.
         *
         * The pipeline is structured as follows:
         *      1. Use the $indexStats stage to gather statistics about each index in all shards.
         *      2. Group all the indexes together and collect them into an array. Also, collect the
         *      names of all the shards in the cluster.
         *      3. Create a new document for each index in the array created by the previous stage.
         *      4. Group all the indexes by name.
         *      5. For each index, create two new fields:
         *          - `missingFromShards`: array of differences between all shards that are expected
         *          to have the index and the shards that actually contain the index.
         *          - `inconsistentProperties`: array of differences between the properties of each
         *          index across all shards.
         *      6. Filter out indexes that are consistent across all shards.
         *      7. Project the final result.
         *
         * Note: step 4 contains a workaround for a spurious InconsistentIndex that can arise in
         * mixed-version clusters. Pre-7.3 nodes persisted 'expireAfterSeconds' using the BSON
         * type of the user-supplied value (e.g. 3600 -> int, 3600.5 -> double), while 7.3+ nodes
         * always truncate to integer. When FCV is updated to 9.0, all the 'expireAfterSeconds'
         * values on disk are normalized to integer. However, for previous FCV values, the
         * inconsistency can still happen. As part of the workaround, we pass
         * '$$tolerateExpireAfterSecondsTypeMismatch' from the caller: when true, all numeric
         * 'expireAfterSeconds' values are cast to long before comparison so that semantically-
         * equivalent values stored with different types compare equal; when false, CMC flags type
         * mismatches as usual. The caller derives the boolean from
         * 'featureFlagStrictExpireAfterSecondsTypeChecking' (FCV-gated at 9.0), so by default the
         * workaround is on below FCV 9.0 and off at FCV >= 9.0.
         *
         * TODO (SERVER-126991): Remove the workaround in step 4 once 9.0 becomes last LTS.
         */
        auto rawPipelineBSON = fromjson(
            R"({pipeline: [
			{$indexStats: {}},
			{$group: {
					_id: null,
					indexDoc: {$push: '$$ROOT'},
					allShards: {$addToSet: '$shard'}
			}},
			{$unwind: '$indexDoc'},
			{$group: {
					'_id': '$indexDoc.name',
					'shards': {$push: '$indexDoc.shard'},)"
            // TODO (SERVER-126991): Remove this $map block and replace it with the original
            //   'specs': {$push: {$objectToArray: {$ifNull: ['$indexDoc.spec', {}]}}},
            // once 9.0 becomes last LTS. See the function-level note for more info.
            R"(
					'specs': {$push: {$map: {
						input: {$objectToArray: {$ifNull: ['$indexDoc.spec', {}]}},
						as: 'kv',
						in: {$cond: {
							if: {$and: [
								{$eq: ['$$kv.k', 'expireAfterSeconds']},
								{$isNumber: '$$kv.v'},
								'$$tolerateExpireAfterSecondsTypeMismatch'
							]},
							then: {k: '$$kv.k',
								v: {$convert: {
									input: '$$kv.v', to: 'long',
									onError: '$$kv.v'}}},
							else: '$$kv'
						}}
					}}},)"
            R"(
					'allShards': {$first: '$allShards'}
			}},
			{$project: {
				missingFromShards: {$setDifference: ['$allShards', '$shards']},
				inconsistentProperties: {
					$setDifference: [
						{$reduce: {
							input: '$specs',
							initialValue: {$arrayElemAt: ['$specs', 0]},
							in: {$setUnion: ['$$value', '$$this']}}},
						{$reduce: {
							input: '$specs',
							initialValue: {$arrayElemAt: ['$specs', 0]},
							in: {$setIntersection: ['$$value', '$$this']}
						}}
					]
				}
			}},
			{$match: {
				$expr: {
					$or: [
						{$gt: [{$size: '$missingFromShards'}, 0]},
						{$gt: [{$size: '$inconsistentProperties'}, 0]
						}
					]
				}
			}},
			{$project: {
				'_id': 0,
				indexName: '$$ROOT._id',
				inconsistentProperties: 1,
				missingFromShards: 1
			}}
		]})");
        return parsePipelineFromBSON(rawPipelineBSON.firstElement());
    }();

    // TODO (SERVER-126991): Remove tolerateExpireAfterSecondsTypeMismatch and related handling
    // once 9.0 becomes last LTS.
    const bool tolerateExpireAfterSecondsTypeMismatch =
        !feature_flags::gStrictExpireAfterSecondsTypeChecking.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    std::vector<MetadataInconsistencyItem> indexIncons;
    for (const auto& coll : collections) {
        const auto& nss = coll.getNss();

        AggregateCommandRequest aggRequest{nss, rawPipelineStages};
        aggRequest.setLet(BSON("tolerateExpireAfterSecondsTypeMismatch"
                               << tolerateExpireAfterSecondsTypeMismatch));

        std::vector<BSONObj> results = _runExhaustiveAggregation(
            opCtx, nss, aggRequest, "Check sharded indexes consistency across shards"sv);

        indexIncons.reserve(results.size());
        for (auto&& rawIndexIncon : results) {
            indexIncons.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kInconsistentIndex,
                InconsistentIndexDetails{nss, std::move(rawIndexIncon)}));
        }
    }
    return indexIncons;
}

std::vector<MetadataInconsistencyItem> checkCollectionMetadataConsistencyAcrossShards(
    OperationContext* opCtx, const std::vector<CollectionType>& collections) {

    const auto getRawPipelineStages = [&](const NamespaceString& nss) {
        auto catalogEntryOnPrimaryShard =
            MongoProcessInterface::create(opCtx)->getCatalogEntry(opCtx, nss);

        /**
         * The following pipeline is used to check the collection metadata consistency across shards
         * of the given collection. In particular, it checks that all shards owning chunks of a
         * collection and the DBPrimary of that collection have the same collection metadata
         * (excluding indexes, whose consistency is checked separately).
         * The DBPrimary shard must always have the collection created locally and its collection
         * metadata must be consistent with other shards regardless if the DBPrimary shard owns
         * chunks or not.
         * Note that here we aren't checking if the collection is missing on any of
         * those shards, this is already done by
         * metadata_consistency_util::checkCollectionMetadataConsistency().
         *
         * The pipeline is structured as follows:
         *      1. Use the $listCatalog stage to gather the collection metadata from all shards
         *      owning chunks.
         *      2. Since $listCatalog only targets shards owning chunks, we may skip checking the
         *      existence of the collection on the DBPrimary shard, where the collection must also
         *      exist. Therefore, in this step we are appending the catalog entry obtained from
         *      the DBPrimary shard to the list of documents returned by $listCatalog. To do so, we
         *      need to concatenate the following 4 stages: $group, $project, $unwind and
         *      $replaceWith.
         *      3. Keep just the two meaningful fields for our purpose: `md` and `shard`.
         *      4. Then, we split the pipeline in two sub-pipelines using a facet stage. The first
         *      sub-pipeline finds inconsistencies within the collection options (`md.options`),
         *      while the second sub-pipeline finds any other inconsistency in the metadata outside
         *      `md.options` and `md.indexes`. This split lets us keep backwards compatibility, and
         *      classify both kinds of inconsistencies separately.
         *      5. Within each sub-pipeline: Group by collection options/metadata in order to
         *      detect inconsistencies between shards. We will end up having one document per every
         *      different collection options/metadata found.
         *      6. Finally, within each sub-pipeline, rename the `_id` field to `options`/`md` to
         *      deliver the inconsistency to the user (if any).
         *
         *      This is an example of the results obtained if there is a collection options
         *      mismatch between shard0 and shard1,shard2:
         *          Inconsistency type: CollectionOptionsMismatch
         *          Inconsistency details: [
         *              {
         *                options: <optionsA>,
         *                shards: [shard0]
         *              },
         *              {
         *                options: <optionsB>,
         *                shards: [shard1,shard2]
         *              }
         *          ]
         *
         *      This is an example of the results obtained if there is a collection auxiliary
         *      metadata mismatch between shard0,shard1 and shard2:
         *          Inconsistency type: CollectionAuxiliaryMetadataMismatch
         *          Inconsistency details: [
         *              {
         *                collectionMetadata: <metadataA>,
         *                shards: [shard0,shard1]
         *              },
         *              {
         *                collectionMetadata: <metadataB>,
         *                shards: [shard2]
         *              }
         *          ]
         */
        std::vector<BSONObj> pipeline;
        pipeline.emplace_back(fromjson(R"(
            {$listCatalog: {}})"));
        if (catalogEntryOnPrimaryShard) {
            pipeline.emplace_back(fromjson(R"(
                {$group: {
                    _id: 0,
                    docs: { $push: "$$ROOT" }
                }})"));
            pipeline.emplace_back(
                BSON("$project" << BSON(
                         "docs" << BSON("$concatArrays" << BSON_ARRAY(
                                            "$docs" << BSON_ARRAY(BSON(
                                                "$literal" << *catalogEntryOnPrimaryShard)))))));
            pipeline.emplace_back(fromjson(R"(
                { $unwind: '$docs' })"));
            pipeline.emplace_back(fromjson(R"(
                { $replaceWith: '$docs' })"));
        }
        simulateCatalogTopLevelMetadataInconsistency.execute([&](const auto&) {
            // Generates a CollectionAuxiliaryMetadataMismatch inconsistency by simulating that
            // $listCatalog returns a top-level field which is inconsistent across shards.
            pipeline.emplace_back(fromjson(R"(
                {$addFields: {
                    'md.testOnlyInconsistentField': '$shard'
                }})"));
        });
        pipeline.emplace_back(fromjson(R"(
            {$project: {
                md: '$md',
                shard: '$shard'
            }})"));
        // Ignore inconsistencies in the legacy timeseries flags. Due to SERVER-91195, those flags
        // have been deprecated and will be removed. At the same time, they can become inconsistent
        // in various scenarios, such as movePrimary or FCV downgrades.
        // TODO (SERVER-101423): Remove tsBucketingParametersHaveChanged once 9.0 becomes last LTS.
        // TODO (SERVER-96831): Remove tsBucketsMayHaveMixedSchemaData field once it's removed.
        pipeline.emplace_back(fromjson(R"(
            {$project: {
                'md.timeseriesBucketingParametersHaveChanged': 0,
                'md.timeseriesBucketsMayHaveMixedSchemaData': 0
            }})"));
        // TODO (SERVER-91702): Remove the exclusion once the race with downgrade is fixed.
        if (const auto& provider =
                rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
            !provider.shouldUseReplicatedRecordIds()) {
            pipeline.emplace_back(fromjson(R"(
                {$project: {
                    'md.recordIdsReplicated': 0
                }})"));
        }
        pipeline.emplace_back(fromjson(R"(
            {$facet: {
                options: [
                    {$group: {
                        _id: '$md.options',
                        shards: {$addToSet: '$shard'}
                    }},
                    {$project: {
                        _id: 0,
                        options: "$_id",
                        shards: 1
                    }}
                ],
                auxiliaryMetadata: [
                    {$project: {
                        "md.options": 0,
                        "md.indexes": 0
                    }},
                    {$group: {
                        _id: "$md",
                        shards: { $addToSet: "$shard" }
                    }},
                    {$project: {
                        _id: 0,
                        md: "$_id",
                        shards: 1
                    }}
                ]
            }})"));
        return pipeline;
    };

    std::vector<MetadataInconsistencyItem> inconsistencies;
    for (const auto& coll : collections) {
        const auto& nss = coll.getNss();
        AggregateCommandRequest aggRequest{nss, getRawPipelineStages(nss)};

        std::vector<BSONObj> facetedResult = _runExhaustiveAggregation(
            opCtx, nss, aggRequest, "Check collection metadata consistency across shards"sv);

        // Even though the last stage of the aggregation is a $facet, the aggregation runner will
        // return an empty vector if aggregation fails due to an inconsistency reported elsewhere.
        if (facetedResult.empty())
            continue;
        tassert(9089900,
                "Expected collection metadata consistency check aggregation to return one document",
                facetedResult.size() == 1);

        // Every element on result's vector contains a unique collection option across the cluster
        // for the given collection. Below are listed the 3 different scenarios we can face:
        //     A) `results` is empty. Which means that the collection is missing on all the shards.
        //        This inconsistency is caught under `checkCollectionMetadataConsistency()`, so we
        //        don't take any action here.
        //     B) `results` size is 1: There is only one unique collection option across the
        //        cluster. This is the expected behavior and means that the collection options are
        //        consistent for the given collection.
        //     C) `results` size is greater than 1: There are 2 or more shards differing on their
        //        collection options, therefore we will return an inconsistency.
        //
        auto optionsField = facetedResult.front().getField("options");
        tassert(9089901,
                "Expected collection metadata check document to contain an 'options' field",
                !optionsField.eoo());
        std::vector<BSONObj> optionsResults;
        for (auto elem : optionsField.Array()) {
            optionsResults.emplace_back(elem.Obj());
        }
        if (optionsResults.size() > 1) {
            // Case where two or more shards have different collection options.
            const auto severity =
                boost::make_optional(nss == NamespaceString::kLogicalSessionsNamespace,
                                     MetadataInconsistencySeverityEnum::kLow);
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionOptionsMismatch,
                CollectionOptionsMismatchDetails{nss, std::move(optionsResults)},
                severity));
        }

        // The same reasoning applies to metadata inconsistencies outside the collection options.
        auto auxiliaryMetadataField = facetedResult.front().getField("auxiliaryMetadata");
        tassert(9089902,
                "Expected collection metadata check document to have an 'auxiliaryMetadata' field",
                !auxiliaryMetadataField.eoo());
        std::vector<BSONObj> auxiliaryMetadataResults;
        for (auto elem : auxiliaryMetadataField.Array()) {
            auxiliaryMetadataResults.emplace_back(elem.Obj());
        }
        if (auxiliaryMetadataResults.size() > 1) {
            // Case where two or more shards have different collection auxiliary metadata.
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionAuxiliaryMetadataMismatch,
                CollectionAuxiliaryMetadataMismatchDetails{nss,
                                                           std::move(auxiliaryMetadataResults)}));
        }
    }
    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkChunksConsistency(OperationContext* opCtx,
                                                              const CollectionType& collection) {
    tassert(9996600,
            "This method must run on the 'config' server.",
            ShardingState::get(opCtx)->shardId() == ShardId::kConfigServerId);

    DBDirectClient client{opCtx};
    // We need to read at snapshot readConcern, set it in the opCtx for DBDirectClient.
    const auto scopedReadConcern = setSnapshotReadConcernIfNeeded(opCtx);
    const auto chunksCursor = _getCollectionChunksCursor(&client, collection);

    const auto& uuid = collection.getUuid();
    const auto& nss = collection.getNss();
    const auto shardKeyPattern = ShardKeyPattern{collection.getKeyPattern()};
    std::vector<MetadataInconsistencyItem> inconsistencies;
    size_t totalChunks = 0;
    ChunkType previousChunk, firstChunk;

    while (chunksCursor->more()) {
        const auto chunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            chunksCursor->nextSafe(), collection.getEpoch(), collection.getTimestamp()));
        totalChunks++;

        const bool chunkHistoryEmpty = chunk.getHistory().empty();
        if (chunkHistoryEmpty) {
            const std::string errMsg = str::stream()
                << "The " << ChunkType::history() << " field is empty";
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kCorruptedChunkHistory,
                CorruptedChunkHistoryDetails{nss, uuid, chunk.toConfigBSON(), errMsg}));

        } else {
            if (chunk.getHistory().front().getShard() != chunk.getShard()) {
                std::string errMsg = str::stream()
                    << "The first element in the history for this chunk must be the owning shard "
                    << chunk.getShard() << " but it is "
                    << chunk.getHistory().front().getShard().toString();
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kCorruptedChunkHistory,
                    CorruptedChunkHistoryDetails{nss, uuid, chunk.toConfigBSON(), errMsg}));
            }

            const bool onCurrentShardSinceMissing = !chunk.getOnCurrentShardSince().has_value();
            if (onCurrentShardSinceMissing) {
                const std::string errMsg = str::stream()
                    << "The " << ChunkType::onCurrentShardSince() << " field is missing";
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kCorruptedChunkHistory,
                    CorruptedChunkHistoryDetails{nss, uuid, chunk.toConfigBSON(), errMsg}));
            } else if (chunk.getHistory().front().getValidAfter() !=
                       *chunk.getOnCurrentShardSince()) {
                std::string errMsg = str::stream()
                    << "The " << ChunkHistoryBase::kValidAfterFieldName
                    << " for the first element in the history"
                    << " must match the value of " << ChunkType::onCurrentShardSince();
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kCorruptedChunkHistory,
                    CorruptedChunkHistoryDetails{nss, uuid, chunk.toConfigBSON(), errMsg}));
            }
        }

        if (!shardKeyPattern.isShardKey(chunk.getMin()) ||
            !shardKeyPattern.isShardKey(chunk.getMax())) {
            inconsistencies.emplace_back(
                makeInconsistency(MetadataInconsistencyTypeEnum::kCorruptedChunkShardKey,
                                  CorruptedChunkShardKeyDetails{
                                      nss, uuid, chunk.toConfigBSON(), shardKeyPattern.toBSON()}));
        }

        // Skip the first iteration as we need to compare the current chunk with the previous one.
        if (totalChunks == 1) {
            firstChunk = chunk;
            previousChunk = chunk;
            continue;
        }

        auto cmp = previousChunk.getMax().woCompare(chunk.getMin());
        if (cmp < 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableRangeGap,
                RoutingTableRangeGapDetails{
                    nss, uuid, previousChunk.toConfigBSON(), chunk.toConfigBSON()}));
        } else if (cmp > 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap,
                RoutingTableRangeOverlapDetails{
                    nss, uuid, previousChunk.toConfigBSON(), chunk.toConfigBSON()}));
        }

        previousChunk = std::move(chunk);
    }

    const ChunkType lastChunk = previousChunk;

    if (collection.getUnsplittable() && totalChunks > 1) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kTrackedUnshardedCollectionHasMultipleChunks,
            TrackedUnshardedCollectionHasMultipleChunksDetails{
                nss, collection.getUuid(), int(totalChunks)}));
    }
    // Check if the first and last chunk have MinKey and MaxKey respectively
    if (!totalChunks) {
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kMissingRoutingTable,
                              MissingRoutingTableDetails{nss, uuid}));
    } else {
        const BSONObj& minKeyObj = firstChunk.getMin();
        const auto globalMin = shardKeyPattern.getKeyPattern().globalMin();
        if (minKeyObj.woCompare(shardKeyPattern.getKeyPattern().globalMin()) != 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey,
                RoutingTableMissingMinKeyDetails{nss, uuid, minKeyObj, globalMin}));
        }

        const BSONObj& maxKeyObj = lastChunk.getMax();
        const auto globalMax = shardKeyPattern.getKeyPattern().globalMax();
        if (maxKeyObj.woCompare(globalMax) != 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey,
                RoutingTableMissingMaxKeyDetails{nss, uuid, maxKeyObj, globalMax}));
        }
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkZonesConsistency(OperationContext* opCtx,
                                                             const CollectionType& collection,
                                                             const std::vector<TagsType>& zones) {
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

std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistency(
    OperationContext* opCtx, const DatabaseType& dbInGlobalCatalog, RSNodeMode rsMode) {
    const auto dbName = dbInGlobalCatalog.getDbName();
    const auto dbVersionInGlobalCatalog = dbInGlobalCatalog.getVersion();
    const auto primaryShard = dbInGlobalCatalog.getPrimary();

    // TODO (SERVER-98118): Unconditionally return the inconsistencies found when we check for the
    // database metadata consistency optimistically - without serializing with the FCV.

    // Happy path: Check the consistency of the database metadata without serializing with the FCV.
    // In the most probable case, there is no concurrent FCV downgrade that could interfere with
    // this check and potentially result in false positives.
    //
    // If the database metadata is checked during an FCV downgrade, the execution may begin under
    // the assumption that shards are database-authoritative, but complete after the downgrade,
    // when the shard is no longer database-authoritative. This leads to fewer guarantees — for
    // example, the shard catalog may not be in sync with the global catalog.

    if (checkDatabaseMetadataConsistencyInShardCatalog(
            opCtx, dbName, dbVersionInGlobalCatalog, primaryShard, rsMode)
            .empty()) {
        return {};
    }

    // Fallback path: Recheck database metadata consistency, this time serializing with the FCV.
    // This ensures that there are no concurrent FCV downgrades that might incorrectly invalidate
    // the assumption that the shard catalog is authoritative.

    FixedFCVRegion fixedFcvRegion(opCtx);

    if (!feature_flags::gAuthoritativeShardsCRUD.isEnabled(VersionContext::getDecoration(opCtx),
                                                           fixedFcvRegion->acquireFCVSnapshot())) {
        return {};
    }

    return checkDatabaseMetadataConsistencyInShardCatalog(
        opCtx, dbName, dbVersionInGlobalCatalog, primaryShard, rsMode);
}

namespace {

// Returns a MetadataInconsistencyItem for each config collection on this shard matching 'filter'.
std::vector<MetadataInconsistencyItem> checkConfigCollectionsDoNotExistLocally(
    OperationContext* opCtx,
    MetadataInconsistencyTypeEnum inconsistencyType,
    BSONObj filter,
    bool validateNonEmptyAndConfigSvrFcvStable = false) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    DBDirectClient client(opCtx);
    for (const auto& collInfo : client.getCollectionInfos(DatabaseName::kConfig, filter)) {
        const auto nss =
            NamespaceStringUtil::deserialize(DatabaseName::kConfig, collInfo["name"].str());
        // TODO(SERVER-98118): remove this branch once 9.0 is last LTS
        // For the Authoritative Shard catalog collections, we have the following edge cases:
        // - For config.shard.catalog.databases, the config server may enter kUpgrading and insert
        //   documents to it while the shard is still fully downgraded.
        // - For config.shard.catalog.collections/chunks, each shard creates the collections before
        //   entering kUpgrading, so those collections may exist (but be empty) on fully downgraded.
        // Therefore we only flag an inconsistency if the collection is non-empty & the configsvr's
        // FCV is stable (in addition to the caller making sure the shard is fully downgraded).
        // Then we can be sure we only report unexpected documents on a fully downgraded cluster.
        if (validateNonEmptyAndConfigSvrFcvStable) {
            auto readConfigServerFCVDocument = [&] {
                auto response = uassertStatusOK(
                    Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                        opCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern),
                        NamespaceString::kServerConfigurationNamespace,
                        BSON("_id" << multiversion::kParameterName),
                        BSONObj{},
                        1 /* limit */));
                tassert(
                    13070900,
                    "Could not find the featureCompatibilityVersion document on the config server",
                    !response.docs.empty());
                return FeatureCompatibilityVersionDocument::parse(response.docs.front());
            };

            const auto initialConfigFCV = readConfigServerFCVDocument();
            if (initialConfigFCV.getTargetVersion() || client.findOne(nss, BSONObj{}).isEmpty() ||
                readConfigServerFCVDocument() != initialConfigFCV) {
                continue;
            }
        }
        inconsistencies.emplace_back(makeInconsistency(
            inconsistencyType,
            UnexpectedShardCatalogCollectionDetails{nss, ShardingState::get(opCtx)->shardId()}));
    }

    return inconsistencies;
}

}  // namespace

std::vector<MetadataInconsistencyItem> checkShardCatalogCollectionsConsistentWithAuthoritativeness(
    OperationContext* opCtx) {
    auto result = tryCheckUnderStableFCV(opCtx, [&](ServerGlobalParams::FCVSnapshot fcvSnapshot) {
        const auto accessLevel = sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
            VersionContext::getDecoration(opCtx), fcvSnapshot);

        if (accessLevel == AuthoritativeMetadataAccessLevelEnum::kNone) {
            // Fully downgraded: authoritative shard catalog collections must not exist.
            return checkConfigCollectionsDoNotExistLocally(
                opCtx,
                MetadataInconsistencyTypeEnum::kAuthoritativeShardCatalogCollectionsPresent,
                BSON("name" << BSON(
                         "$in" << BSON_ARRAY(
                             NamespaceString::kConfigShardCatalogDatabasesNamespace.coll()
                             << NamespaceString::kConfigShardCatalogCollectionsNamespace.coll()
                             << NamespaceString::kConfigShardCatalogChunksNamespace.coll()))),
                true /* validateNonEmptyAndConfigSvrFcvStable */);
        }

        if (accessLevel == AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed) {
            // Fully upgraded: legacy shard catalog cache collections must not exist.
            return checkConfigCollectionsDoNotExistLocally(
                opCtx,
                MetadataInconsistencyTypeEnum::kLegacyShardCacheCollectionsPresent,
                BSON("$or" << BSON_ARRAY(
                         BSON("name" << BSON(
                                  "$in" << BSON_ARRAY(
                                      NamespaceString::kConfigCacheDatabasesNamespace.coll()
                                      << NamespaceString::kShardConfigCollectionsNamespace.coll())))
                         << BSON("name" << BSONRegEx(R"(^cache\.chunks\.)")))));
        }

        // kWritesAllowed only happens on a non-steady FCV, so this should not happen.
        tasserted(12797700, "Authoritativeness was kWritesAllowed but expected stable FCV");
    });

    return result.value_or(std::vector<MetadataInconsistencyItem>{});
}

std::pair<std::vector<MetadataInconsistencyItem>, boost::optional<Timestamp>>
runCheckMetadataConsistencyOnParticipant(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const ShardId& primaryShardId,
                                         bool checkRangeDeletionIndexes,
                                         bool checkIndexes,
                                         RSNodeMode rsMode) {
    const auto shardId = ShardingState::get(opCtx)->shardId();
    const auto commandLevel = getCommandLevel(nss);

    tassert(1011703,
            str::stream() << "Unexpected parameter during the internal execution of "
                             "checkMetadataConsistency command. The shard server was expecting to "
                             "receive a database or collection level parameter, but received "
                          << idl::serialize(commandLevel) << " with namespace "
                          << nss.toStringForErrorMsg(),
            commandLevel == MetadataConsistencyCommandLevelEnum::kCollectionLevel ||
                commandLevel == MetadataConsistencyCommandLevelEnum::kDatabaseLevel);

    uassert(ErrorCodes::InvalidOptions,
            "Range deletion missing shard key index inconsistency check is not supported with the "
            "current FCV. Upgrade to the highest FCV for performing the check.",
            !checkRangeDeletionIndexes ||
                feature_flags::gCheckRangeDeletionsWithMissingShardKeyIndex.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    auto [collCatalogSnapshot, localCatalogCollections, snapshotTimestamp] = [&] {
        if (rsMode != RSNodeMode::kDelayedSecondary) {
            return std::tuple_cat(getCatalogAndCollections(opCtx, nss, commandLevel),
                                  std::tuple<boost::optional<Timestamp>>{boost::none});
        } else {
            return getConsistentSnapshot(opCtx, nss, commandLevel);
        }
    }();

    boost::optional<ScopedReadConcern> scopedReadConcern;
    stdx::unordered_set<NamespaceString> collectionsUnderCs;
    if (rsMode == RSNodeMode::kDelayedSecondary) {
        invariant(snapshotTimestamp);
        repl::ReadConcernArgs snapshotReadConcern{repl::ReadConcernLevel::kSnapshotReadConcern};
        snapshotReadConcern.setArgsAtClusterTimeForSnapshot(*snapshotTimestamp);
        scopedReadConcern.emplace(opCtx, std::move(snapshotReadConcern));

        collectionsUnderCs = getCollectionsUnderCriticalSection(opCtx, localCatalogCollections);

        LOGV2(
            12922300,
            "Running _shardsvrCheckMetadataConsistencySecondaryParticipant on a delayed secondary",
            "timestamp"_attr = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime());
    }

    // Get the list of collections from configsvr sorted by namespace
    const auto configsvrCollections = getCollectionsListFromConfigServer(opCtx, nss, commandLevel);

    const auto currentPrimaryShardId = [&] {
        if (rsMode != RSNodeMode::kDelayedSecondary) {
            return primaryShardId;
        }
        // On a delayed secondary, the primaryShardId as reported in the command arguments can be
        // stale. In that case, trust the CSRS and get the primary shardID from it.
        try {
            return ShardId(
                Grid::get(opCtx)
                    ->catalogClient()
                    ->getDatabase(opCtx, nss.dbName(), getReadConcernForConfigServer(opCtx))
                    .getPrimary());
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& e) {
            // The collection didn't exist at the current snapshot, so return a snapshot error.
            uasserted(ErrorCodes::SnapshotUnavailable,
                      fmt::format("Database not found at the current optime: {}", e.toString()));
        }
    }();

    auto inconsistencies = checkCollectionMetadataConsistency(opCtx,
                                                              shardId,
                                                              currentPrimaryShardId,
                                                              configsvrCollections,
                                                              collCatalogSnapshot,
                                                              localCatalogCollections,
                                                              checkRangeDeletionIndexes,
                                                              checkIndexes,
                                                              rsMode,
                                                              collectionsUnderCs);

    // If this is the primary shard of the db coordinate index check across shards
    if (shardId == currentPrimaryShardId) {
        // Inter-shard checks don't make sense on RS secondaries.
        if (rsMode == RSNodeMode::kPrimary) {
            if (checkIndexes) {
                auto indexInconsistencies =
                    metadata_consistency_util::checkIndexesConsistencyAcrossShards(
                        opCtx, configsvrCollections);
                inconsistencies.insert(inconsistencies.end(),
                                       std::make_move_iterator(indexInconsistencies.begin()),
                                       std::make_move_iterator(indexInconsistencies.end()));
            }

            auto collMetadataInconsistencies =
                checkCollectionMetadataConsistencyAcrossShards(opCtx, configsvrCollections);
            inconsistencies.insert(inconsistencies.end(),
                                   std::make_move_iterator(collMetadataInconsistencies.begin()),
                                   std::make_move_iterator(collMetadataInconsistencies.end()));
        }

        if (feature_flags::gAuthoritativeShardsCRUD.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
            !nss.isConfigDB()) {
            const auto dbInGlobalCatalog = Grid::get(opCtx)->catalogClient()->getDatabase(
                opCtx, nss.dbName(), getReadConcernForConfigServer(opCtx));

            auto dbMetadataInconsistencies =
                checkDatabaseMetadataConsistency(opCtx, dbInGlobalCatalog, rsMode);
            inconsistencies.insert(inconsistencies.end(),
                                   std::make_move_iterator(dbMetadataInconsistencies.begin()),
                                   std::make_move_iterator(dbMetadataInconsistencies.end()));
        }
    }

    return {std::move(inconsistencies), snapshotTimestamp};
}

}  // namespace metadata_consistency_util
}  // namespace mongo
