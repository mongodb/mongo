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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_manager.h"

#include "mongo/base/owned_pointer_vector.h"
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

namespace mongo {
namespace {

// Used to generate sequence numbers to assign to each newly created RoutingTableHistory
AtomicWord<unsigned> nextCMSequenceNumber(0);

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
        if (chunk->getLastmod() > chunks.back()->getLastmod()) {
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

}  // namespace

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
            shardVersionIt =
                shardVersions.emplace(currentRangeShardId, _collectionVersion.epoch()).first;
        }

        auto& maxShardVersion = shardVersionIt->second.shardVersion;

        current =
            std::find_if(current,
                         _chunkMap.cend(),
                         [&currentRangeShardId, &maxShardVersion](const auto& currentChunk) {
                             if (currentChunk->getShardIdAt(boost::none) != currentRangeShardId)
                                 return true;

                             if (currentChunk->getLastmod() > maxShardVersion)
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
        invariant(firstMin.is_initialized());
        invariant(lastMax.is_initialized());

        checkAllElementsAreOfType(MinKey, firstMin.get());
        checkAllElementsAreOfType(MaxKey, lastMax.get());
    }

    return shardVersions;
}

void ChunkMap::appendChunk(const std::shared_ptr<ChunkInfo>& chunk) {
    appendChunkTo(_chunkMap, chunk);

    _collectionVersion = std::max(_collectionVersion, chunk->getLastmod());
}

std::shared_ptr<ChunkInfo> ChunkMap::findIntersectingChunk(const BSONObj& shardKey) const {
    const auto it = _findIntersectingChunk(shardKey);

    if (it != _chunkMap.end())
        return *it;

    return std::shared_ptr<ChunkInfo>();
}

void validateChunk(const std::shared_ptr<ChunkInfo>& chunk, const ChunkVersion& version) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Changed chunk " << chunk->toString()
                          << " has epoch different from that of the collection " << version.epoch(),
            version.epoch() == chunk->getLastmod().epoch());

    invariant(chunk->getLastmod() >= version);
}

ChunkMap ChunkMap::createMerged(const std::vector<std::shared_ptr<ChunkInfo>>& changedChunks) {
    size_t chunkMapIndex = 0;
    size_t changedChunkIndex = 0;

    ChunkMap updatedChunkMap(getVersion().epoch(), _chunkMap.size() + changedChunks.size());

    while (chunkMapIndex < _chunkMap.size() || changedChunkIndex < changedChunks.size()) {
        if (chunkMapIndex >= _chunkMap.size()) {
            validateChunk(changedChunks[changedChunkIndex], getVersion());
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

            validateChunk(changedChunk, getVersion());
            updatedChunkMap.appendChunk(changedChunk);
        } else {
            updatedChunkMap.appendChunk(_chunkMap[chunkMapIndex++]);
        }
    }

    return updatedChunkMap;
}

BSONObj ChunkMap::toBSON() const {
    BSONObjBuilder builder;

    builder.append("startingVersion"_sd, getVersion().toBSON());
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

ShardVersionTargetingInfo::ShardVersionTargetingInfo(const OID& epoch)
    : shardVersion(0, 0, epoch) {}

RoutingTableHistory::RoutingTableHistory(NamespaceString nss,
                                         boost::optional<UUID> uuid,
                                         KeyPattern shardKeyPattern,
                                         std::unique_ptr<CollatorInterface> defaultCollator,
                                         bool unique,
                                         ChunkMap chunkMap)
    : _sequenceNumber(nextCMSequenceNumber.addAndFetch(1)),
      _nss(std::move(nss)),
      _uuid(uuid),
      _shardKeyPattern(shardKeyPattern),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
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

Chunk ChunkManager::findIntersectingChunk(const BSONObj& shardKey, const BSONObj& collation) const {
    const bool hasSimpleCollation = (collation.isEmpty() && !_rt->getDefaultCollator()) ||
        SimpleBSONObjComparator::kInstance.evaluate(collation == CollationSpec::kSimpleSpec);
    if (!hasSimpleCollation) {
        for (BSONElement elt : shardKey) {
            uassert(ErrorCodes::ShardKeyNotFound,
                    str::stream() << "Cannot target single shard due to collation of key "
                                  << elt.fieldNameStringData() << " for namespace " << getns(),
                    !CollationIndexKey::isCollatableType(elt.type()));
        }
    }

    auto chunkInfo = _rt->findIntersectingChunk(shardKey);

    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Cannot target single shard using key " << shardKey
                          << " for namespace " << getns(),
            chunkInfo && chunkInfo->containsKey(shardKey));

    return Chunk(*chunkInfo, _clusterTime);
}

bool ChunkManager::keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const {
    if (shardKey.isEmpty())
        return false;

    auto chunkInfo = _rt->findIntersectingChunk(shardKey);
    if (!chunkInfo)
        return false;

    invariant(chunkInfo->containsKey(shardKey));

    return chunkInfo->getShardIdAt(_clusterTime) == shardId;
}

void ChunkManager::getShardIdsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                                       const BSONObj& query,
                                       const BSONObj& collation,
                                       std::set<ShardId>* shardIds) const {
    auto qr = std::make_unique<QueryRequest>(_rt->getns());
    qr->setFilter(query);

    if (auto uuid = getUUID())
        expCtx->uuid = uuid;

    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    } else if (_rt->getDefaultCollator()) {
        auto defaultCollator = _rt->getDefaultCollator();
        qr->setCollation(defaultCollator->getSpec().toBSON());
        expCtx->setCollator(defaultCollator->clone());
    }

    auto cq = uassertStatusOK(
        CanonicalQuery::canonicalize(expCtx->opCtx,
                                     std::move(qr),
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    // Fast path for targeting equalities on the shard key.
    auto shardKeyToFind = _rt->getShardKeyPattern().extractShardKeyFromQuery(*cq);
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
    IndexBounds bounds = getIndexBoundsForQuery(_rt->getShardKeyPattern().toBSON(), *cq);

    // Transforms bounds for each shard key field into full shard key ranges
    // for example :
    //   Key { a : 1, b : 1 }
    //   Bounds { a : [1, 2), b : [3, 4) }
    //   => Ranges { a : 1, b : 3 } => { a : 2, b : 4 }
    BoundList ranges = _rt->getShardKeyPattern().flattenBounds(bounds);

    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        getShardIdsForRange(it->first /*min*/, it->second /*max*/, shardIds);

        // Once we know we need to visit all shards no need to keep looping.
        // However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->_shardVersions.size()) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        _rt->forEachChunk([&](const std::shared_ptr<ChunkInfo>& chunkInfo) {
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

    _rt->forEachOverlappingChunk(min, max, true, [&](auto& chunkInfo) {
        shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards. However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->_shardVersions.size()) {
            return false;
        }

        return true;
    });
}

bool ChunkManager::rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const {
    bool overlapFound = false;

    _rt->forEachOverlappingChunk(range.getMin(), range.getMax(), false, [&](auto& chunkInfo) {
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

    _rt->forEachChunk(
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
        IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
        return bounds;
    }

    // Similarly, ignore GEO_NEAR queries in planning, since we do not have geo indexes on mongos.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::GEO_NEAR)) {
        IndexBounds bounds;
        IndexBoundsBuilder::allValuesBounds(key, &bounds);
        return bounds;
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

    auto plannerResult = QueryPlanner::plan(canonicalQuery, plannerParams);
    if (plannerResult.getStatus().code() != ErrorCodes::NoQueryExecutionPlans) {
        auto solutions = uassertStatusOK(std::move(plannerResult));

        // Pick any solution that has non-trivial IndexBounds. bounds.size() == 0 represents a
        // trivial IndexBounds where none of the fields' values are bounded.
        for (auto&& soln : solutions) {
            IndexBounds bounds = collapseQuerySolution(soln->root.get());
            if (bounds.size() > 0) {
                return bounds;
            }
        }
    }

    // We cannot plan the query without collection scan, so target to all shards.
    IndexBounds bounds;
    IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
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
        return collapseQuerySolution(node->children.front());
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

    for (std::vector<QuerySolutionNode*>::const_iterator it = node->children.begin();
         it != node->children.end();
         it++) {
        // The first branch under OR
        if (it == node->children.begin()) {
            invariant(bounds.size() == 0);
            bounds = collapseQuerySolution(*it);
            if (bounds.size() == 0) {  // Got unexpected node in query solution tree
                return IndexBounds();
            }
            continue;
        }

        IndexBounds childBounds = collapseQuerySolution(*it);
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
        // have a version of (0, 0, epoch)
        return ChunkVersion(0, 0, _chunkMap.getVersion().epoch());
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

std::shared_ptr<RoutingTableHistory> RoutingTableHistory::makeNew(
    NamespaceString nss,
    boost::optional<UUID> uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    OID epoch,
    const std::vector<ChunkType>& chunks) {
    return RoutingTableHistory(std::move(nss),
                               std::move(uuid),
                               std::move(shardKeyPattern),
                               std::move(defaultCollator),
                               std::move(unique),
                               ChunkMap{epoch})
        .makeUpdated(chunks);
}

std::shared_ptr<RoutingTableHistory> RoutingTableHistory::makeUpdated(
    const std::vector<ChunkType>& changedChunks) {

    // It's possible for there to be one chunk in changedChunks without the routing table having
    // changed. We skip copying the ChunkMap when this happens.
    if (changedChunks.size() == 1 && changedChunks[0].getVersion() == getVersion()) {
        return shared_from_this();
    }

    auto changedChunkInfos = flatten(changedChunks);
    auto chunkMap = _chunkMap.createMerged(changedChunkInfos);

    // If at least one diff was applied, the metadata is correct, but it might not have changed so
    // in this case there is no need to recreate the chunk manager.
    //
    // NOTE: In addition to the above statement, it is also important that we return the same chunk
    // manager object, because the write commands' code relies on changes of the chunk manager's
    // sequence number to detect batch writes not making progress because of chunks moving across
    // shards too frequently.
    if (chunkMap.getVersion() == getVersion()) {
        return shared_from_this();
    }

    return std::shared_ptr<RoutingTableHistory>(
        new RoutingTableHistory(_nss,
                                _uuid,
                                KeyPattern(getShardKeyPattern().getKeyPattern()),
                                CollatorInterface::cloneCollator(getDefaultCollator()),
                                isUnique(),
                                std::move(chunkMap)));
}

}  // namespace mongo
