/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Used to generate sequence numbers to assign to each newly created ChunkManager
AtomicUInt32 nextCMSequenceNumber(0);

void checkAllElementsAreOfType(BSONType type, const BSONObj& o) {
    for (const auto&& element : o) {
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Not all elements of " << o << " are of type " << typeName(type),
                element.type() == type);
    }
}

}  // namespace

ChunkManager::ChunkManager(NamespaceString nss,
                           KeyPattern shardKeyPattern,
                           std::unique_ptr<CollatorInterface> defaultCollator,
                           bool unique,
                           ChunkMap chunkMap,
                           ChunkVersion collectionVersion)
    : _sequenceNumber(nextCMSequenceNumber.addAndFetch(1)),
      _nss(std::move(nss)),
      _shardKeyPattern(shardKeyPattern),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _chunkMap(std::move(chunkMap)),
      _chunkMapViews(_constructChunkMapViews(collectionVersion.epoch(), _chunkMap)),
      _collectionVersion(collectionVersion) {}

ChunkManager::~ChunkManager() = default;

std::shared_ptr<Chunk> ChunkManager::findIntersectingChunk(const BSONObj& shardKey,
                                                           const BSONObj& collation) const {
    const bool hasSimpleCollation = (collation.isEmpty() && !_defaultCollator) ||
        SimpleBSONObjComparator::kInstance.evaluate(collation == CollationSpec::kSimpleSpec);
    if (!hasSimpleCollation) {
        for (BSONElement elt : shardKey) {
            uassert(ErrorCodes::ShardKeyNotFound,
                    str::stream() << "Cannot target single shard due to collation of key "
                                  << elt.fieldNameStringData(),
                    !CollationIndexKey::isCollatableType(elt.type()));
        }
    }

    const auto it = _chunkMap.upper_bound(shardKey);
    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Cannot target single shard using key " << shardKey,
            it != _chunkMap.end() && it->second->containsKey(shardKey));

    return it->second;
}

std::shared_ptr<Chunk> ChunkManager::findIntersectingChunkWithSimpleCollation(
    const BSONObj& shardKey) const {
    return findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);
}

void ChunkManager::getShardIdsForQuery(OperationContext* txn,
                                       const BSONObj& query,
                                       const BSONObj& collation,
                                       std::set<ShardId>* shardIds) const {
    auto qr = stdx::make_unique<QueryRequest>(_nss);
    qr->setFilter(query);

    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    } else if (_defaultCollator) {
        qr->setCollation(_defaultCollator->getSpec().toBSON());
    }

    std::unique_ptr<CanonicalQuery> cq =
        uassertStatusOK(CanonicalQuery::canonicalize(txn, std::move(qr), ExtensionsCallbackNoop()));

    // Query validation
    if (QueryPlannerCommon::hasNode(cq->root(), MatchExpression::GEO_NEAR)) {
        uasserted(13501, "use geoNear command rather than $near query");
    }

    // Fast path for targeting equalities on the shard key.
    auto shardKeyToFind = _shardKeyPattern.extractShardKeyFromQuery(*cq);
    if (!shardKeyToFind.isEmpty()) {
        try {
            auto chunk = findIntersectingChunk(shardKeyToFind, collation);
            shardIds->insert(chunk->getShardId());
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
    IndexBounds bounds = getIndexBoundsForQuery(_shardKeyPattern.toBSON(), *cq);

    // Transforms bounds for each shard key field into full shard key ranges
    // for example :
    //   Key { a : 1, b : 1 }
    //   Bounds { a : [1, 2), b : [3, 4) }
    //   => Ranges { a : 1, b : 3 } => { a : 2, b : 4 }
    BoundList ranges = _shardKeyPattern.flattenBounds(bounds);

    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        getShardIdsForRange(it->first /*min*/, it->second /*max*/, shardIds);

        // once we know we need to visit all shards no need to keep looping
        if (shardIds->size() == _chunkMapViews.shardVersions.size()) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        shardIds->insert(_chunkMapViews.chunkRangeMap.begin()->second.shardId);
    }
}

void ChunkManager::getShardIdsForRange(const BSONObj& min,
                                       const BSONObj& max,
                                       std::set<ShardId>* shardIds) const {
    auto it = _chunkMapViews.chunkRangeMap.upper_bound(min);
    auto end = _chunkMapViews.chunkRangeMap.upper_bound(max);

    // The chunk range map must always cover the entire key space
    invariant(it != _chunkMapViews.chunkRangeMap.end());

    // We need to include the last chunk
    if (end != _chunkMapViews.chunkRangeMap.cend()) {
        ++end;
    }

    for (; it != end; ++it) {
        shardIds->insert(it->second.shardId);

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards.
        if (shardIds->size() == _chunkMapViews.shardVersions.size()) {
            break;
        }
    }
}

void ChunkManager::getAllShardIds(std::set<ShardId>* all) const {
    std::transform(_chunkMapViews.shardVersions.begin(),
                   _chunkMapViews.shardVersions.end(),
                   std::inserter(*all, all->begin()),
                   [](const ShardVersionMap::value_type& pair) { return pair.first; });
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

    OwnedPointerVector<QuerySolution> solutions;
    Status status = QueryPlanner::plan(canonicalQuery, plannerParams, &solutions.mutableVector());
    uassert(status.code(), status.reason(), status.isOK());

    IndexBounds bounds;

    for (std::vector<QuerySolution*>::const_iterator it = solutions.begin();
         bounds.size() == 0 && it != solutions.end();
         it++) {
        // Try next solution if we failed to generate index bounds, i.e. bounds.size() == 0
        bounds = collapseQuerySolution((*it)->root.get());
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

bool ChunkManager::compatibleWith(const ChunkManager& other, const ShardId& shardName) const {
    // Return true if the shard version is the same in the two chunk managers
    // TODO: This doesn't need to be so strong, just major vs
    return other.getVersion(shardName).equals(getVersion(shardName));
}

ChunkVersion ChunkManager::getVersion(const ShardId& shardName) const {
    auto it = _chunkMapViews.shardVersions.find(shardName);
    if (it == _chunkMapViews.shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch)
        return ChunkVersion(0, 0, _collectionVersion.epoch());
    }

    return it->second;
}

std::string ChunkManager::toString() const {
    StringBuilder sb;
    sb << "ChunkManager: " << _nss.ns() << " key:" << _shardKeyPattern.toString() << '\n';

    for (const auto& entry : _chunkMap) {
        sb << "\t" << entry.second->toString() << '\n';
    }

    return sb.str();
}

ChunkManager::ChunkMapViews ChunkManager::_constructChunkMapViews(const OID& epoch,
                                                                  const ChunkMap& chunkMap) {
    invariant(!chunkMap.empty());

    ChunkRangeMap chunkRangeMap =
        SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<ShardAndChunkRange>();

    ShardVersionMap shardVersions;

    ChunkMap::const_iterator current = chunkMap.cbegin();

    while (current != chunkMap.cend()) {
        const auto& firstChunkInRange = current->second;

        // Tracks the max shard version for the shard on which the current range will reside
        auto shardVersionIt = shardVersions.find(firstChunkInRange->getShardId());
        if (shardVersionIt == shardVersions.end()) {
            shardVersionIt =
                shardVersions.emplace(firstChunkInRange->getShardId(), ChunkVersion(0, 0, epoch))
                    .first;
        }

        auto& maxShardVersion = shardVersionIt->second;

        current = std::find_if(
            current,
            chunkMap.cend(),
            [&firstChunkInRange, &maxShardVersion](const ChunkMap::value_type& chunkMapEntry) {
                const auto& currentChunk = chunkMapEntry.second;

                if (currentChunk->getShardId() != firstChunkInRange->getShardId())
                    return true;

                if (currentChunk->getLastmod() > maxShardVersion)
                    maxShardVersion = currentChunk->getLastmod();

                return false;
            });

        const auto rangeLast = std::prev(current);

        const BSONObj rangeMin = firstChunkInRange->getMin();
        const BSONObj rangeMax = rangeLast->second->getMax();

        const auto insertResult = chunkRangeMap.insert(std::make_pair(
            rangeMax, ShardAndChunkRange{{rangeMin, rangeMax}, firstChunkInRange->getShardId()}));
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Metadata contains two chunks with the same max value "
                              << rangeMax,
                insertResult.second);

        const auto& insertIterator = insertResult.first;

        if (insertIterator != chunkRangeMap.begin()) {
            // Make sure there are no gaps in the ranges
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Gap or an overlap between ranges "
                                  << insertIterator->second.range.toString()
                                  << " and "
                                  << std::prev(insertIterator)->second.range.toString(),
                    SimpleBSONObjComparator::kInstance.evaluate(std::prev(insertIterator)->first ==
                                                                rangeMin));
        }

        // If a shard has chunks it must have a shard version, otherwise we have an invalid chunk
        // somewhere, which should have been caught at chunk load time
        invariant(maxShardVersion.isSet());
    }

    invariant(!chunkRangeMap.empty());
    invariant(!shardVersions.empty());

    checkAllElementsAreOfType(MinKey, chunkRangeMap.begin()->second.min());
    checkAllElementsAreOfType(MaxKey, chunkRangeMap.rbegin()->first);

    return {std::move(chunkRangeMap), std::move(shardVersions)};
}

}  // namespace mongo
