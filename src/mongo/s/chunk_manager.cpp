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

#include "mongo/s/chunk_manager.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/s/chunk_writes_tracker.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

bool allElementsAreOfType(BSONType type, const BSONObj& obj) {
    for (auto&& elem : obj) {
        if (elem.type() != type) {
            return false;
        }
    }
    return true;
}

void checkAllElementsAreOfType(BSONType type, const BSONObj& o) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Not all elements of " << o << " are of type " << typeName(type),
            allElementsAreOfType(type, o));
}

void appendChunkTo(std::vector<std::shared_ptr<ChunkInfo>>& chunks,
                   const std::shared_ptr<ChunkInfo>& chunk) {
    if (!chunks.empty() && chunk->getRange().overlaps(chunks.back()->getRange())) {
        if (chunks.back()->getLastmod().isOlderThan(chunk->getLastmod())) {
            chunks.pop_back();
            chunks.push_back(chunk);
        }
    } else {
        chunks.push_back(chunk);
    }
}

// This function processes the passed in chunks by removing the older versions of any overlapping
// chunks. The resulting chunks must be ordered by the maximum bound and not have any
// overlapping chunks. In order to process the original set of chunks correctly which may have
// chunks from older versions of the map that overlap, this algorithm would need to sort by
// ascending minimum bounds before processing it. However, since we want to take advantage of the
// precomputed KeyString representations of the maximum bounds, this function implements the same
// algorithm by reverse sorting the chunks by the maximum before processing but then must
// reverse the resulting collection before it is returned.
std::vector<std::shared_ptr<ChunkInfo>> flatten(const std::vector<ChunkType>& changedChunks) {
    if (changedChunks.empty())
        return std::vector<std::shared_ptr<ChunkInfo>>();

    std::vector<std::shared_ptr<ChunkInfo>> changedChunkInfos(changedChunks.size());
    std::transform(changedChunks.begin(),
                   changedChunks.end(),
                   changedChunkInfos.begin(),
                   [](const auto& c) { return std::make_shared<ChunkInfo>(c); });

    std::sort(changedChunkInfos.begin(), changedChunkInfos.end(), [](const auto& a, const auto& b) {
        return a->getMaxKeyString() > b->getMaxKeyString();
    });

    std::vector<std::shared_ptr<ChunkInfo>> flattened;
    flattened.reserve(changedChunkInfos.size());
    flattened.push_back(changedChunkInfos[0]);

    for (size_t i = 1; i < changedChunkInfos.size(); ++i) {
        appendChunkTo(flattened, changedChunkInfos[i]);
    }

    std::reverse(flattened.begin(), flattened.end());

    return flattened;
}

void validateChunkIsNotOlderThan(const std::shared_ptr<ChunkInfo>& chunk,
                                 const ChunkVersion& version) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Changed chunk " << chunk->toString()
                          << " has timestamp different from that of the collection "
                          << version.getTimestamp(),
            version.getTimestamp() == chunk->getLastmod().getTimestamp());

    uassert(626840,
            str::stream()
                << "Changed chunk " << chunk->toString()
                << " doesn't have version that's greater or equal than that of the collection "
                << version.toString(),
            version.isOlderOrEqualThan(chunk->getLastmod()));
}

}  // namespace

ChunkMap::ChunkMap(OID epoch, const Timestamp& timestamp, size_t initialCapacity)
    : _collectionVersion({epoch, timestamp}, {0, 0}) {
    _chunkMap.reserve(initialCapacity);
}

ShardVersionMap ChunkMap::constructShardVersionMap() const {
    ShardVersionMap shardVersions;
    ChunkVector::const_iterator current = _chunkMap.cbegin();

    boost::optional<BSONObj> firstMin = boost::none;
    boost::optional<BSONObj> lastMax = boost::none;

    while (current != _chunkMap.cend()) {
        const auto& firstChunkInRange = *current;
        const auto& currentRangeShardId = firstChunkInRange->getShardIdAt(boost::none);

        // Tracks the max shard version for the shard on which the current range will reside
        auto shardVersionIt = shardVersions.find(currentRangeShardId);
        if (shardVersionIt == shardVersions.end()) {
            shardVersionIt = shardVersions
                                 .emplace(std::piecewise_construct,
                                          std::forward_as_tuple(currentRangeShardId),
                                          std::forward_as_tuple(_collectionVersion.epoch(),
                                                                _collectionVersion.getTimestamp()))
                                 .first;
        }

        auto& maxShardVersion = shardVersionIt->second.shardVersion;

        current =
            std::find_if(current,
                         _chunkMap.cend(),
                         [&currentRangeShardId, &maxShardVersion](const auto& currentChunk) {
                             if (currentChunk->getShardIdAt(boost::none) != currentRangeShardId)
                                 return true;

                             if (maxShardVersion.isOlderThan(currentChunk->getLastmod()))
                                 maxShardVersion = currentChunk->getLastmod();

                             return false;
                         });

        const auto rangeLast = *std::prev(current);

        const auto& rangeMin = firstChunkInRange->getMin();
        const auto& rangeMax = rangeLast->getMax();

        // Check the continuity of the chunks map
        if (lastMax && !SimpleBSONObjComparator::kInstance.evaluate(*lastMax == rangeMin)) {
            if (SimpleBSONObjComparator::kInstance.evaluate(*lastMax < rangeMin))
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "Gap exists in the routing table between chunks "
                                        << findIntersectingChunk(*lastMax)->getRange().toString()
                                        << " and " << rangeLast->getRange().toString());
            else
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "Overlap exists in the routing table between chunks "
                                        << findIntersectingChunk(*lastMax)->getRange().toString()
                                        << " and " << rangeLast->getRange().toString());
        }

        if (!firstMin)
            firstMin = rangeMin;

        lastMax = rangeMax;

        // If a shard has chunks it must have a shard version, otherwise we have an invalid chunk
        // somewhere, which should have been caught at chunk load time
        invariant(maxShardVersion.isSet());
    }

    if (!_chunkMap.empty()) {
        invariant(!shardVersions.empty());
        invariant(firstMin.has_value());
        invariant(lastMax.has_value());

        checkAllElementsAreOfType(MinKey, firstMin.value());
        checkAllElementsAreOfType(MaxKey, lastMax.value());
    }

    return shardVersions;
}

void ChunkMap::appendChunk(const std::shared_ptr<ChunkInfo>& chunk) {
    appendChunkTo(_chunkMap, chunk);
    const auto chunkVersion = chunk->getLastmod();
    if (_collectionVersion.isOlderThan(chunkVersion)) {
        _collectionVersion = chunkVersion;
    }
}

std::shared_ptr<ChunkInfo> ChunkMap::findIntersectingChunk(const BSONObj& shardKey) const {
    const auto it = _findIntersectingChunk(shardKey);

    if (it != _chunkMap.end())
        return *it;

    return std::shared_ptr<ChunkInfo>();
}

ChunkMap ChunkMap::createMerged(
    const std::vector<std::shared_ptr<ChunkInfo>>& changedChunks) const {
    size_t chunkMapIndex = 0;
    size_t changedChunkIndex = 0;

    ChunkMap updatedChunkMap(
        getVersion().epoch(), getVersion().getTimestamp(), _chunkMap.size() + changedChunks.size());

    while (chunkMapIndex < _chunkMap.size() || changedChunkIndex < changedChunks.size()) {
        if (chunkMapIndex >= _chunkMap.size()) {
            validateChunkIsNotOlderThan(changedChunks[changedChunkIndex], getVersion());
            updatedChunkMap.appendChunk(changedChunks[changedChunkIndex++]);
            continue;
        }

        if (changedChunkIndex >= changedChunks.size()) {
            updatedChunkMap.appendChunk(_chunkMap[chunkMapIndex++]);
            continue;
        }

        auto overlap = _chunkMap[chunkMapIndex]->getRange().overlaps(
            changedChunks[changedChunkIndex]->getRange());

        if (overlap) {
            auto& changedChunk = changedChunks[changedChunkIndex++];
            auto& chunkInfo = _chunkMap[chunkMapIndex];

            auto bytesInReplacedChunk = chunkInfo->getWritesTracker()->getBytesWritten();
            changedChunk->getWritesTracker()->addBytesWritten(bytesInReplacedChunk);

            validateChunkIsNotOlderThan(changedChunk, getVersion());
            updatedChunkMap.appendChunk(changedChunk);
        } else {
            updatedChunkMap.appendChunk(_chunkMap[chunkMapIndex++]);
        }
    }

    return updatedChunkMap;
}

BSONObj ChunkMap::toBSON() const {
    BSONObjBuilder builder;

    getVersion().serialize("startingVersion"_sd, &builder);
    builder.append("chunkCount", static_cast<int64_t>(_chunkMap.size()));

    {
        BSONArrayBuilder arrayBuilder(builder.subarrayStart("chunks"_sd));
        for (const auto& chunk : _chunkMap) {
            arrayBuilder.append(chunk->toString());
        }
    }

    return builder.obj();
}

ChunkMap::ChunkVector::const_iterator ChunkMap::_findIntersectingChunk(const BSONObj& shardKey,
                                                                       bool isMaxInclusive) const {
    auto shardKeyString = ShardKeyPattern::toKeyString(shardKey);

    if (!isMaxInclusive) {
        return std::lower_bound(_chunkMap.begin(),
                                _chunkMap.end(),
                                shardKey,
                                [&shardKeyString](const auto& chunkInfo, const BSONObj& shardKey) {
                                    return chunkInfo->getMaxKeyString() < shardKeyString;
                                });
    } else {
        return std::upper_bound(_chunkMap.begin(),
                                _chunkMap.end(),
                                shardKey,
                                [&shardKeyString](const BSONObj& shardKey, const auto& chunkInfo) {
                                    return shardKeyString < chunkInfo->getMaxKeyString();
                                });
    }
}

std::pair<ChunkMap::ChunkVector::const_iterator, ChunkMap::ChunkVector::const_iterator>
ChunkMap::_overlappingBounds(const BSONObj& min, const BSONObj& max, bool isMaxInclusive) const {
    const auto itMin = _findIntersectingChunk(min);
    const auto itMax = [&]() {
        auto it = _findIntersectingChunk(max, isMaxInclusive);
        return it == _chunkMap.end() ? it : ++it;
    }();

    return {itMin, itMax};
}

ShardVersionTargetingInfo::ShardVersionTargetingInfo(const OID& epoch, const Timestamp& timestamp)
    : shardVersion({epoch, timestamp}, {0, 0}) {}

RoutingTableHistory::RoutingTableHistory(
    NamespaceString nss,
    UUID uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    boost::optional<uint64_t> maxChunkSizeBytes,
    bool allowMigrations,
    ChunkMap chunkMap)
    : _nss(std::move(nss)),
      _uuid(std::move(uuid)),
      _shardKeyPattern(std::move(shardKeyPattern)),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _timeseriesFields(std::move(timeseriesFields)),
      _reshardingFields(std::move(reshardingFields)),
      _maxChunkSizeBytes(maxChunkSizeBytes),
      _allowMigrations(allowMigrations),
      _chunkMap(std::move(chunkMap)),
      _shardVersions(_chunkMap.constructShardVersionMap()) {}

void RoutingTableHistory::setShardStale(const ShardId& shardId) {
    if (gEnableFinerGrainedCatalogCacheRefresh) {
        auto it = _shardVersions.find(shardId);
        if (it != _shardVersions.end()) {
            it->second.isStale.store(true);
        }
    }
}

void RoutingTableHistory::setAllShardsRefreshed() {
    if (gEnableFinerGrainedCatalogCacheRefresh) {
        for (auto& [shard, targetingInfo] : _shardVersions) {
            targetingInfo.isStale.store(false);
        }
    }
}

Chunk ChunkManager::findIntersectingChunk(const BSONObj& shardKey,
                                          const BSONObj& collation,
                                          bool bypassIsFieldHashedCheck) const {
    const bool hasSimpleCollation = (collation.isEmpty() && !_rt->optRt->getDefaultCollator()) ||
        SimpleBSONObjComparator::kInstance.evaluate(collation == CollationSpec::kSimpleSpec);
    if (!hasSimpleCollation) {
        for (BSONElement elt : shardKey) {
            // We must assume that if the field is specified as "hashed" in the shard key pattern,
            // then the hash value could have come from a collatable type.
            const bool isFieldHashed =
                (_rt->optRt->getShardKeyPattern().isHashedPattern() &&
                 _rt->optRt->getShardKeyPattern().getHashedField().fieldNameStringData() ==
                     elt.fieldNameStringData());

            // If we want to skip the check in the special case where the _id field is hashed and
            // used as the shard key, set bypassIsFieldHashedCheck. This assumes that a request with
            // a query that contains an _id field can target a specific shard.
            uassert(ErrorCodes::ShardKeyNotFound,
                    str::stream() << "Cannot target single shard due to collation of key "
                                  << elt.fieldNameStringData() << " for namespace "
                                  << _rt->optRt->nss(),
                    !CollationIndexKey::isCollatableType(elt.type()) &&
                        (!isFieldHashed || bypassIsFieldHashedCheck));
        }
    }

    auto chunkInfo = _rt->optRt->findIntersectingChunk(shardKey);

    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Cannot target single shard using key " << shardKey
                          << " for namespace " << _rt->optRt->nss(),
            chunkInfo && chunkInfo->containsKey(shardKey));

    return Chunk(*chunkInfo, _clusterTime);
}

bool ChunkManager::keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const {
    if (shardKey.isEmpty())
        return false;

    auto chunkInfo = _rt->optRt->findIntersectingChunk(shardKey);
    if (!chunkInfo)
        return false;

    invariant(chunkInfo->containsKey(shardKey));

    return chunkInfo->getShardIdAt(_clusterTime) == shardId;
}

void ChunkManager::getShardIdsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                                       const BSONObj& query,
                                       const BSONObj& collation,
                                       std::set<ShardId>* shardIds) const {
    auto findCommand = std::make_unique<FindCommandRequest>(_rt->optRt->nss());
    findCommand->setFilter(query.getOwned());

    expCtx->uuid = getUUID();

    if (!collation.isEmpty()) {
        findCommand->setCollation(collation.getOwned());
    } else if (_rt->optRt->getDefaultCollator()) {
        auto defaultCollator = _rt->optRt->getDefaultCollator();
        findCommand->setCollation(defaultCollator->getSpec().toBSON());
        expCtx->setCollator(defaultCollator->clone());
    }

    auto cq = uassertStatusOK(
        CanonicalQuery::canonicalize(expCtx->opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    // Fast path for targeting equalities on the shard key.
    auto shardKeyToFind = _rt->optRt->getShardKeyPattern().extractShardKeyFromQuery(*cq);
    if (!shardKeyToFind.isEmpty()) {
        try {
            auto chunk = findIntersectingChunk(shardKeyToFind, collation);
            shardIds->insert(chunk.getShardId());
            return;
        } catch (const DBException&) {
            // The query uses multiple shards
        }
    }

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    IndexBounds bounds = getIndexBoundsForQuery(_rt->optRt->getShardKeyPattern().toBSON(), *cq);

    // Transforms bounds for each shard key field into full shard key ranges
    // for example :
    //   Key { a : 1, b : 1 }
    //   Bounds { a : [1, 2), b : [3, 4) }
    //   => Ranges { a : 1, b : 3 } => { a : 2, b : 4 }
    BoundList ranges = _rt->optRt->getShardKeyPattern().flattenBounds(bounds);

    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        getShardIdsForRange(it->first /*min*/, it->second /*max*/, shardIds);

        // Once we know we need to visit all shards no need to keep looping.
        // However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->optRt->_shardVersions.size()) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        _rt->optRt->forEachChunk([&](const std::shared_ptr<ChunkInfo>& chunkInfo) {
            shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));
            return false;
        });
    }
}

void ChunkManager::getShardIdsForRange(const BSONObj& min,
                                       const BSONObj& max,
                                       std::set<ShardId>* shardIds) const {
    // If our range is [MinKey, MaxKey], we can simply return all shard ids right away. However,
    // this optimization does not apply when we are reading from a snapshot because _shardVersions
    // contains shards with chunks and is built based on the last refresh. Therefore, it is
    // possible for _shardVersions to have fewer entries if a shard no longer owns chunks when it
    // used to at _clusterTime.
    if (!_clusterTime && allElementsAreOfType(MinKey, min) && allElementsAreOfType(MaxKey, max)) {
        getAllShardIds(shardIds);
        return;
    }

    _rt->optRt->forEachOverlappingChunk(min, max, true, [&](auto& chunkInfo) {
        shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards. However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->optRt->_shardVersions.size()) {
            return false;
        }

        return true;
    });
}

bool ChunkManager::rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const {
    bool overlapFound = false;

    _rt->optRt->forEachOverlappingChunk(
        range.getMin(), range.getMax(), false, [&](auto& chunkInfo) {
            if (chunkInfo->getShardIdAt(_clusterTime) == shardId) {
                overlapFound = true;
                return false;
            }

            return true;
        });

    return overlapFound;
}

boost::optional<Chunk> ChunkManager::getNextChunkOnShard(const BSONObj& shardKey,
                                                         const ShardId& shardId) const {
    boost::optional<Chunk> chunk;

    _rt->optRt->forEachChunk(
        [&](auto& chunkInfo) {
            if (chunkInfo->getShardIdAt(_clusterTime) == shardId) {
                chunk.emplace(*chunkInfo, _clusterTime);
                return false;
            }
            return true;
        },
        shardKey);

    return chunk;
}

ShardId ChunkManager::getMinKeyShardIdWithSimpleCollation() const {
    auto minKey = getShardKeyPattern().getKeyPattern().globalMin();
    return findIntersectingChunkWithSimpleCollation(minKey).getShardId();
}

void RoutingTableHistory::getAllShardIds(std::set<ShardId>* all) const {
    std::transform(_shardVersions.begin(),
                   _shardVersions.end(),
                   std::inserter(*all, all->begin()),
                   [](const ShardVersionMap::value_type& pair) { return pair.first; });
}

int RoutingTableHistory::getNShardsOwningChunks() const {
    return _shardVersions.size();
}

IndexBounds ChunkManager::getIndexBoundsForQuery(const BSONObj& key,
                                                 const CanonicalQuery& canonicalQuery) {
    // $text is not allowed in planning since we don't have text index on mongos.
    // TODO: Treat $text query as a no-op in planning on mongos. So with shard key {a: 1},
    //       the query { a: 2, $text: { ... } } will only target to {a: 2}.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::TEXT)) {
        IndexBounds bounds;
        IndexBoundsBuilder::allValuesBounds(key, &bounds, false);  // [minKey, maxKey]
        return bounds;
    }

    // Similarly, ignore GEO_NEAR queries in planning, since we do not have geo indexes on mongos.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::GEO_NEAR)) {
        // If the GEO_NEAR predicate is a child of AND, remove the GEO_NEAR and continue building
        // bounds. Currently a CanonicalQuery can have at most one GEO_NEAR expression, and only at
        // the top-level, so this check is sufficient.
        auto geoIdx = [](auto root) -> boost::optional<size_t> {
            if (root->matchType() == MatchExpression::AND) {
                for (size_t i = 0; i < root->numChildren(); ++i) {
                    if (MatchExpression::GEO_NEAR == root->getChild(i)->matchType()) {
                        return boost::make_optional(i);
                    }
                }
            }
            return boost::none;
        }(canonicalQuery.root());

        if (!geoIdx) {
            IndexBounds bounds;
            IndexBoundsBuilder::allValuesBounds(key, &bounds, false);
            return bounds;
        }

        canonicalQuery.root()->getChildVector()->erase(
            canonicalQuery.root()->getChildVector()->begin() + geoIdx.value());
    }

    // Consider shard key as an index
    std::string accessMethod = IndexNames::findPluginName(key);
    dassert(accessMethod == IndexNames::BTREE || accessMethod == IndexNames::HASHED);
    const auto indexType = IndexNames::nameToType(accessMethod);

    // Use query framework to generate index bounds
    QueryPlannerParams plannerParams;
    // Must use "shard key" index
    plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;
    IndexEntry indexEntry(key,
                          indexType,
                          IndexDescriptor::kLatestIndexVersion,
                          // The shard key index cannot be multikey.
                          false,
                          // Empty multikey paths, since the shard key index cannot be multikey.
                          MultikeyPaths{},
                          // Empty multikey path set, since the shard key index cannot be multikey.
                          {},
                          false /* sparse */,
                          false /* unique */,
                          IndexEntry::Identifier{"shardkey"},
                          nullptr /* filterExpr */,
                          BSONObj(),
                          nullptr, /* collator */
                          nullptr /* projExec */);
    plannerParams.indices.push_back(std::move(indexEntry));

    auto statusWithMultiPlanSolns = QueryPlanner::plan(canonicalQuery, plannerParams);
    if (statusWithMultiPlanSolns.getStatus().code() != ErrorCodes::NoQueryExecutionPlans) {
        auto solutions = uassertStatusOK(std::move(statusWithMultiPlanSolns));

        // Pick any solution that has non-trivial IndexBounds. bounds.size() == 0 represents a
        // trivial IndexBounds where none of the fields' values are bounded.
        for (auto&& soln : solutions) {
            IndexBounds bounds = collapseQuerySolution(soln->root());
            if (bounds.size() > 0) {
                return bounds;
            }
        }
    }

    // We cannot plan the query without collection scan, so target to all shards.
    IndexBounds bounds;
    IndexBoundsBuilder::allValuesBounds(key, &bounds, false);  // [minKey, maxKey]
    return bounds;
}

IndexBounds ChunkManager::collapseQuerySolution(const QuerySolutionNode* node) {
    if (node->children.empty()) {
        invariant(node->getType() == STAGE_IXSCAN);

        const IndexScanNode* ixNode = static_cast<const IndexScanNode*>(node);
        return ixNode->bounds;
    }

    if (node->children.size() == 1) {
        // e.g. FETCH -> IXSCAN
        return collapseQuerySolution(node->children.front().get());
    }

    // children.size() > 1, assert it's OR / SORT_MERGE.
    if (node->getType() != STAGE_OR && node->getType() != STAGE_SORT_MERGE) {
        // Unexpected node. We should never reach here.
        LOGV2_ERROR(23833,
                    "could not generate index bounds on query solution tree: {node}",
                    "node"_attr = redact(node->toString()));
        dassert(false);  // We'd like to know this error in testing.

        // Bail out with all shards in production, since this isn't a fatal error.
        return IndexBounds();
    }

    IndexBounds bounds;

    for (auto it = node->children.begin(); it != node->children.end(); it++) {
        // The first branch under OR
        if (it == node->children.begin()) {
            invariant(bounds.size() == 0);
            bounds = collapseQuerySolution(it->get());
            if (bounds.size() == 0) {  // Got unexpected node in query solution tree
                return IndexBounds();
            }
            continue;
        }

        IndexBounds childBounds = collapseQuerySolution(it->get());
        if (childBounds.size() == 0) {
            // Got unexpected node in query solution tree
            return IndexBounds();
        }

        invariant(childBounds.size() == bounds.size());

        for (size_t i = 0; i < bounds.size(); i++) {
            bounds.fields[i].intervals.insert(bounds.fields[i].intervals.end(),
                                              childBounds.fields[i].intervals.begin(),
                                              childBounds.fields[i].intervals.end());
        }
    }

    for (size_t i = 0; i < bounds.size(); i++) {
        IndexBoundsBuilder::unionize(&bounds.fields[i]);
    }

    return bounds;
}

ChunkManager ChunkManager::makeAtTime(const ChunkManager& cm, Timestamp clusterTime) {
    return ChunkManager(cm.dbPrimary(), cm.dbVersion(), cm._rt, clusterTime);
}

bool ChunkManager::allowMigrations() const {
    if (!_rt->optRt)
        return true;
    return _rt->optRt->allowMigrations();
}

bool ChunkManager::allowAutoSplit() const {
    const auto maxChunkSize = maxChunkSizeBytes();
    if (!maxChunkSize)
        return true;

    return *maxChunkSize != 0;
}

boost::optional<uint64_t> ChunkManager::maxChunkSizeBytes() const {
    if (!_rt->optRt)
        return boost::none;
    return _rt->optRt->maxChunkSizeBytes();
}

std::string ChunkManager::toString() const {
    return _rt->optRt ? _rt->optRt->toString() : "UNSHARDED";
}

bool RoutingTableHistory::compatibleWith(const RoutingTableHistory& other,
                                         const ShardId& shardName) const {
    // Return true if the shard version is the same in the two chunk managers
    // TODO: This doesn't need to be so strong, just major vs
    return other.getVersion(shardName) == getVersion(shardName);
}

ChunkVersion RoutingTableHistory::_getVersion(const ShardId& shardName,
                                              bool throwOnStaleShard) const {
    auto it = _shardVersions.find(shardName);
    if (it == _shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch, timestamp)
        const auto collVersion = _chunkMap.getVersion();
        return ChunkVersion({collVersion.epoch(), collVersion.getTimestamp()}, {0, 0});
    }

    if (throwOnStaleShard && gEnableFinerGrainedCatalogCacheRefresh) {
        uassert(ShardInvalidatedForTargetingInfo(_nss),
                "shard has been marked stale",
                !it->second.isStale.load());
    }

    return it->second.shardVersion;
}

ChunkVersion RoutingTableHistory::getVersion(const ShardId& shardName) const {
    return _getVersion(shardName, true);
}

ChunkVersion RoutingTableHistory::getVersionForLogging(const ShardId& shardName) const {
    return _getVersion(shardName, false);
}

std::string RoutingTableHistory::toString() const {
    StringBuilder sb;
    sb << "RoutingTableHistory: " << _nss.ns() << " key: " << _shardKeyPattern.toString() << '\n';

    sb << "Chunks:\n";
    _chunkMap.forEach([&sb](const auto& chunk) {
        sb << "\t" << chunk->toString() << '\n';
        return true;
    });

    sb << "Shard versions:\n";
    for (const auto& entry : _shardVersions) {
        sb << "\t" << entry.first << ": " << entry.second.shardVersion.toString() << '\n';
    }

    return sb.str();
}

RoutingTableHistory RoutingTableHistory::makeNew(
    NamespaceString nss,
    UUID uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    OID epoch,
    const Timestamp& timestamp,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    boost::optional<uint64_t> maxChunkSizeBytes,
    bool allowMigrations,
    const std::vector<ChunkType>& chunks) {

    auto changedChunkInfos = flatten(chunks);
    return RoutingTableHistory(std::move(nss),
                               std::move(uuid),
                               std::move(shardKeyPattern),
                               std::move(defaultCollator),
                               std::move(unique),
                               std::move(timeseriesFields),
                               std::move(reshardingFields),
                               maxChunkSizeBytes,
                               allowMigrations,
                               ChunkMap{epoch, timestamp}.createMerged(changedChunkInfos));
}

// Note that any new parameters added to RoutingTableHistory::makeUpdated() must also be added to
// ShardServerCatalogCacheLoader::_getLoaderMetadata() and copied into the persisted metadata when
// it may overlap with the enqueued metadata.
RoutingTableHistory RoutingTableHistory::makeUpdated(
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    boost::optional<uint64_t> maxChunkSizeBytes,
    bool allowMigrations,
    const std::vector<ChunkType>& changedChunks) const {

    auto changedChunkInfos = flatten(changedChunks);
    auto chunkMap = _chunkMap.createMerged(changedChunkInfos);

    // Only update the same collection.
    invariant(getVersion().isSameCollection(chunkMap.getVersion()));

    return RoutingTableHistory(_nss,
                               _uuid,
                               getShardKeyPattern().getKeyPattern(),
                               CollatorInterface::cloneCollator(getDefaultCollator()),
                               isUnique(),
                               std::move(timeseriesFields),
                               std::move(reshardingFields),
                               maxChunkSizeBytes,
                               allowMigrations,
                               std::move(chunkMap));
}

AtomicWord<uint64_t> ComparableChunkVersion::_epochDisambiguatingSequenceNumSource{1ULL};
AtomicWord<uint64_t> ComparableChunkVersion::_forcedRefreshSequenceNumSource{1ULL};

ComparableChunkVersion ComparableChunkVersion::makeComparableChunkVersion(
    const ChunkVersion& version) {
    return ComparableChunkVersion(_forcedRefreshSequenceNumSource.load(),
                                  version,
                                  _epochDisambiguatingSequenceNumSource.fetchAndAdd(1));
}

ComparableChunkVersion ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh() {
    return ComparableChunkVersion(_forcedRefreshSequenceNumSource.addAndFetch(2) - 1,
                                  boost::none,
                                  _epochDisambiguatingSequenceNumSource.fetchAndAdd(1));
}

void ComparableChunkVersion::setChunkVersion(const ChunkVersion& version) {
    _chunkVersion = version;
}

std::string ComparableChunkVersion::toString() const {
    BSONObjBuilder builder;
    if (_chunkVersion)
        _chunkVersion->serialize("chunkVersion"_sd, &builder);
    else
        builder.append("chunkVersion"_sd, "None");

    builder.append("forcedRefreshSequenceNum"_sd, static_cast<int64_t>(_forcedRefreshSequenceNum));
    builder.append("epochDisambiguatingSequenceNum"_sd,
                   static_cast<int64_t>(_epochDisambiguatingSequenceNum));

    return builder.obj().toString();
}

bool ComparableChunkVersion::operator==(const ComparableChunkVersion& other) const {
    if (_forcedRefreshSequenceNum != other._forcedRefreshSequenceNum)
        return false;  // Values created on two sides of a forced refresh sequence number are always
                       // considered different
    if (_forcedRefreshSequenceNum == 0)
        return true;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                      // they are always equal

    // Relying on the boost::optional<ChunkVersion>::operator== comparison
    return _chunkVersion == other._chunkVersion;
}

bool ComparableChunkVersion::operator<(const ComparableChunkVersion& other) const {
    if (_forcedRefreshSequenceNum < other._forcedRefreshSequenceNum)
        return true;  // Values created on two sides of a forced refresh sequence number are always
                      // considered different
    if (_forcedRefreshSequenceNum > other._forcedRefreshSequenceNum)
        return false;  // Values created on two sides of a forced refresh sequence number are always
                       // considered different
    if (_forcedRefreshSequenceNum == 0)
        return false;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                       // they are always equal
    if (_chunkVersion.has_value() != other._chunkVersion.has_value())
        return _epochDisambiguatingSequenceNum <
            other._epochDisambiguatingSequenceNum;  // One side is not initialised, but the other
                                                    // is, which can only happen if one side is
                                                    // ForForcedRefresh and the other is made from
                                                    // makeComparableChunkVersion. In this case, use
                                                    // the _epochDisambiguatingSequenceNum to see
                                                    // which one is more recent.
    if (!_chunkVersion.has_value())
        return _epochDisambiguatingSequenceNum <
            other._epochDisambiguatingSequenceNum;  // Both sides are not initialised, which can
                                                    // only happen if both were created from
                                                    // ForForcedRefresh. In this case, use the
                                                    // _epochDisambiguatingSequenceNum to see which
                                                    // one is more recent.

    if (_chunkVersion->getTimestamp() == other._chunkVersion->getTimestamp()) {
        if (!_chunkVersion->isSet() && !other._chunkVersion->isSet()) {
            return false;
        } else if (_chunkVersion->isSet() && other._chunkVersion->isSet()) {
            return _chunkVersion->majorVersion() < other._chunkVersion->majorVersion() ||
                (_chunkVersion->majorVersion() == other._chunkVersion->majorVersion() &&
                 _chunkVersion->minorVersion() < other._chunkVersion->minorVersion());
        }
    } else if (_chunkVersion->isSet() && other._chunkVersion->isSet()) {
        return _chunkVersion->getTimestamp() < other._chunkVersion->getTimestamp();
    }
    return _epochDisambiguatingSequenceNum < other._epochDisambiguatingSequenceNum;
}

ShardEndpoint::ShardEndpoint(const ShardId& shardName,
                             boost::optional<ShardVersion> shardVersion,
                             boost::optional<DatabaseVersion> dbVersion)
    : shardName(shardName),
      shardVersion(std::move(shardVersion)),
      databaseVersion(std::move(dbVersion)) {
    if (databaseVersion)
        invariant(shardVersion && *shardVersion == ShardVersion::UNSHARDED());
    else if (shardVersion)
        invariant(*shardVersion != ShardVersion::UNSHARDED());
    else
        invariant(shardName == ShardId::kConfigServerId);
}

}  // namespace mongo
