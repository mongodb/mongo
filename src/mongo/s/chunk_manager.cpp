
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

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
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Used to generate sequence numbers to assign to each newly created RoutingTableHistory
AtomicUInt32 nextCMSequenceNumber(0);

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

std::string extractKeyStringInternal(const BSONObj& shardKeyValue, Ordering ordering) {
    BSONObjBuilder strippedKeyValue;
    for (const auto& elem : shardKeyValue) {
        strippedKeyValue.appendAs(elem, ""_sd);
    }

    KeyString ks(KeyString::Version::V1, strippedKeyValue.done(), ordering);
    return {ks.getBuffer(), ks.getSize()};
}

}  // namespace

RoutingTableHistory::RoutingTableHistory(NamespaceString nss,
                                         boost::optional<UUID> uuid,
                                         KeyPattern shardKeyPattern,
                                         std::unique_ptr<CollatorInterface> defaultCollator,
                                         bool unique,
                                         ChunkInfoMap chunkMap,
                                         ChunkVersion collectionVersion)
    : _sequenceNumber(nextCMSequenceNumber.addAndFetch(1)),
      _nss(std::move(nss)),
      _uuid(uuid),
      _shardKeyPattern(shardKeyPattern),
      _shardKeyOrdering(Ordering::make(_shardKeyPattern.toBSON())),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _chunkMap(std::move(chunkMap)),
      _collectionVersion(collectionVersion),
      _shardVersions(_constructShardVersionMap()) {}

Chunk ChunkManager::findIntersectingChunk(const BSONObj& shardKey,
                                          const BSONObj& collation,
                                          bool bypassIsFieldHashedCheck) const {
    const bool hasSimpleCollation = (collation.isEmpty() && !_rt->getDefaultCollator()) ||
        SimpleBSONObjComparator::kInstance.evaluate(collation == CollationSpec::kSimpleSpec);
    if (!hasSimpleCollation) {
        for (BSONElement elt : shardKey) {
            // We must assume that if the field is specified as "hashed" in the shard key pattern,
            // then the hash value could have come from a collatable type. If we want to skip the
            // check in the special case where the _id field is hashed and used as the shard key,
            // set bypassIsFieldHashedCheck. This assumes that a request with a query that contains
            // an _id field can target a specific shard.
            uassert(ErrorCodes::ShardKeyNotFound,
                    str::stream() << "Cannot target single shard due to collation of key "
                                  << elt.fieldNameStringData()
                                  << " for namespace "
                                  << getns().toString(),
                    !CollationIndexKey::isCollatableType(elt.type()) &&
                        (!_rt->getShardKeyPattern().isHashedPattern() || bypassIsFieldHashedCheck));
        }
    }

    const auto it = _rt->getChunkMap().upper_bound(_rt->_extractKeyString(shardKey));
    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Cannot target single shard using key " << shardKey
                          << " for namespace "
                          << getns().toString(),
            it != _rt->getChunkMap().end() && it->second->containsKey(shardKey));

    return Chunk(*(it->second), _clusterTime);
}

bool ChunkManager::keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const {
    if (shardKey.isEmpty())
        return false;

    const auto it = _rt->getChunkMap().upper_bound(_rt->_extractKeyString(shardKey));
    if (it == _rt->getChunkMap().end())
        return false;

    invariant(it->second->containsKey(shardKey));

    return it->second->getShardIdAt(_clusterTime) == shardId;
}

void ChunkManager::getShardIdsForQuery(OperationContext* opCtx,
                                       const BSONObj& query,
                                       const BSONObj& collation,
                                       std::set<ShardId>* shardIds) const {
    auto qr = stdx::make_unique<QueryRequest>(_rt->getns());
    qr->setFilter(query);

    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    } else if (_rt->getDefaultCollator()) {
        qr->setCollation(_rt->getDefaultCollator()->getSpec().toBSON());
    }

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto cq = uassertStatusOK(
        CanonicalQuery::canonicalize(opCtx,
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

        // once we know we need to visit all shards no need to keep looping
        if (shardIds->size() == _rt->_shardVersions.size()) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        shardIds->insert(_rt->getChunkMap().begin()->second->getShardIdAt(_clusterTime));
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

    const auto bounds = _rt->overlappingRanges(min, max, true);
    for (auto it = bounds.first; it != bounds.second; ++it) {
        shardIds->insert(it->second->getShardIdAt(_clusterTime));

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards.
        if (shardIds->size() == _rt->_shardVersions.size()) {
            break;
        }
    }
}

bool ChunkManager::rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const {
    const auto bounds = _rt->overlappingRanges(range.getMin(), range.getMax(), false);
    const auto it = std::find_if(bounds.first, bounds.second, [this, &shardId](const auto& scr) {
        return scr.second->getShardIdAt(_clusterTime) == shardId;
    });

    return it != bounds.second;
}

ChunkManager::ConstRangeOfChunks ChunkManager::getNextChunkOnShard(const BSONObj& shardKey,
                                                                   const ShardId& shardId) const {
    for (auto it = _rt->getChunkMap().upper_bound(_rt->_extractKeyString(shardKey));
         it != _rt->getChunkMap().end();
         ++it) {
        const auto& chunk = it->second;
        if (chunk->getShardIdAt(_clusterTime) == shardId) {
            const auto begin = it;
            const auto end = ++it;
            return {ConstChunkIterator(begin, _clusterTime), ConstChunkIterator(end, _clusterTime)};
        }
    }

    return {ConstChunkIterator(), ConstChunkIterator()};
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

std::pair<ChunkInfoMap::const_iterator, ChunkInfoMap::const_iterator>
RoutingTableHistory::overlappingRanges(const BSONObj& min,
                                       const BSONObj& max,
                                       bool isMaxInclusive) const {

    const auto itMin = _chunkMap.upper_bound(_extractKeyString(min));
    const auto itMax = [this, &max, isMaxInclusive]() {
        auto it = isMaxInclusive ? _chunkMap.upper_bound(_extractKeyString(max))
                                 : _chunkMap.lower_bound(_extractKeyString(max));
        return it == _chunkMap.end() ? it : ++it;
    }();

    return {itMin, itMax};
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

    // Use query framework to generate index bounds
    QueryPlannerParams plannerParams;
    // Must use "shard key" index
    plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;
    IndexEntry indexEntry(key,
                          accessMethod,
                          false /* multiKey */,
                          MultikeyPaths{},
                          false /* sparse */,
                          false /* unique */,
                          "shardkey",
                          NULL /* filterExpr */,
                          BSONObj(),
                          NULL /* collator */);
    plannerParams.indices.push_back(indexEntry);

    auto solutions = uassertStatusOK(QueryPlanner::plan(canonicalQuery, plannerParams));

    IndexBounds bounds;

    for (auto&& soln : solutions) {
        // Try next solution if we failed to generate index bounds, i.e. bounds.size() == 0
        bounds = collapseQuerySolution(soln->root.get());
    }

    if (bounds.size() == 0) {
        // We cannot plan the query without collection scan, so target to all shards.
        IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
    }
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
        error() << "could not generate index bounds on query solution tree: "
                << redact(node->toString());
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

ChunkVersion RoutingTableHistory::getVersion(const ShardId& shardName) const {
    auto it = _shardVersions.find(shardName);
    if (it == _shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch)
        return ChunkVersion(0, 0, _collectionVersion.epoch());
    }

    return it->second;
}

std::string RoutingTableHistory::toString() const {
    StringBuilder sb;
    sb << "RoutingTableHistory: " << _nss.ns() << " key: " << _shardKeyPattern.toString() << '\n';

    sb << "Chunks:\n";
    for (const auto& chunk : _chunkMap) {
        sb << "\t" << chunk.second->toString() << '\n';
    }

    sb << "Shard versions:\n";
    for (const auto& entry : _shardVersions) {
        sb << "\t" << entry.first << ": " << entry.second.toString() << '\n';
    }

    return sb.str();
}

ShardVersionMap RoutingTableHistory::_constructShardVersionMap() const {
    const OID& epoch = _collectionVersion.epoch();

    ShardVersionMap shardVersions;
    ChunkInfoMap::const_iterator current = _chunkMap.cbegin();

    boost::optional<BSONObj> firstMin = boost::none;
    boost::optional<BSONObj> lastMax = boost::none;

    while (current != _chunkMap.cend()) {
        const auto& firstChunkInRange = current->second;
        const auto& currentRangeShardId = firstChunkInRange->getShardIdAt(boost::none);

        // Tracks the max shard version for the shard on which the current range will reside
        auto shardVersionIt = shardVersions.find(currentRangeShardId);
        if (shardVersionIt == shardVersions.end()) {
            shardVersionIt =
                shardVersions.emplace(currentRangeShardId, ChunkVersion(0, 0, epoch)).first;
        }

        auto& maxShardVersion = shardVersionIt->second;

        current =
            std::find_if(current,
                         _chunkMap.cend(),
                         [&currentRangeShardId,
                          &maxShardVersion](const ChunkInfoMap::value_type& chunkMapEntry) {
                             const auto& currentChunk = chunkMapEntry.second;

                             if (currentChunk->getShardIdAt(boost::none) != currentRangeShardId)
                                 return true;

                             if (currentChunk->getLastmod() > maxShardVersion)
                                 maxShardVersion = currentChunk->getLastmod();

                             return false;
                         });

        const auto rangeLast = std::prev(current);

        const auto& rangeMin = firstChunkInRange->getMin();
        const auto& rangeMax = rangeLast->second->getMax();

        // Check the continuity of the chunks map
        if (lastMax && !SimpleBSONObjComparator::kInstance.evaluate(*lastMax == rangeMin)) {
            if (SimpleBSONObjComparator::kInstance.evaluate(*lastMax < rangeMin))
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream()
                              << "Gap exists in the routing table between chunks "
                              << _chunkMap.at(_extractKeyString(*lastMax))->getRange().toString()
                              << " and "
                              << rangeLast->second->getRange().toString());
            else
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream()
                              << "Overlap exists in the routing table between chunks "
                              << _chunkMap.at(_extractKeyString(*lastMax))->getRange().toString()
                              << " and "
                              << rangeLast->second->getRange().toString());
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

std::string RoutingTableHistory::_extractKeyString(const BSONObj& shardKeyValue) const {
    return extractKeyStringInternal(shardKeyValue, _shardKeyOrdering);
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
                               {},
                               {0, 0, epoch})
        .makeUpdated(chunks);
}

std::shared_ptr<RoutingTableHistory> RoutingTableHistory::makeUpdated(
    const std::vector<ChunkType>& changedChunks) {

    const auto startingCollectionVersion = getVersion();
    auto chunkMap = _chunkMap;

    ChunkVersion collectionVersion = startingCollectionVersion;
    for (const auto& chunk : changedChunks) {
        const auto& chunkVersion = chunk.getVersion();

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Chunk " << chunk.genID(getns(), chunk.getMin())
                              << " has epoch different from that of the collection "
                              << chunkVersion.epoch(),
                collectionVersion.epoch() == chunkVersion.epoch());

        // Chunks must always come in incrementally sorted order
        invariant(chunkVersion >= collectionVersion);
        collectionVersion = chunkVersion;

        const auto chunkMinKeyString = _extractKeyString(chunk.getMin());
        const auto chunkMaxKeyString = _extractKeyString(chunk.getMax());

        // Returns the first chunk with a max key that is > min - implies that the chunk overlaps
        // min
        const auto low = chunkMap.upper_bound(chunkMinKeyString);

        // Returns the first chunk with a max key that is > max - implies that the next chunk cannot
        // not overlap max
        const auto high = chunkMap.upper_bound(chunkMaxKeyString);

        // Erase all chunks from the map, which overlap the chunk we got from the persistent store
        chunkMap.erase(low, high);

        // Insert only the chunk itself
        chunkMap.insert(std::make_pair(chunkMaxKeyString, std::make_shared<ChunkInfo>(chunk)));
    }

    // If at least one diff was applied, the metadata is correct, but it might not have changed so
    // in this case there is no need to recreate the chunk manager.
    //
    // NOTE: In addition to the above statement, it is also important that we return the same chunk
    // manager object, because the write commands' code relies on changes of the chunk manager's
    // sequence number to detect batch writes not making progress because of chunks moving across
    // shards too frequently.
    if (collectionVersion == startingCollectionVersion) {
        return shared_from_this();
    }

    return std::shared_ptr<RoutingTableHistory>(
        new RoutingTableHistory(_nss,
                                _uuid,
                                KeyPattern(getShardKeyPattern().getKeyPattern()),
                                CollatorInterface::cloneCollator(getDefaultCollator()),
                                isUnique(),
                                std::move(chunkMap),
                                collectionVersion));
}

}  // namespace mongo
