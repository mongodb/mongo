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

#include <boost/next_prior.hpp>
#include <vector>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::map;
using std::pair;
using std::set;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

namespace {

// Used to generate sequence numbers to assign to each newly created ChunkManager
AtomicUInt32 nextCMSequenceNumber(0);

/**
 * This is an adapter so we can use config diffs - mongos and mongod do them slightly differently.
 *
 * The mongos adapter here tracks all shards, and stores ranges by (max, Chunk) in the map.
 */
class CMConfigDiffTracker : public ConfigDiffTracker<shared_ptr<Chunk>> {
public:
    CMConfigDiffTracker(const std::string& ns,
                        RangeMap* currMap,
                        ChunkVersion* maxVersion,
                        MaxChunkVersionMap* maxShardVersions,
                        ChunkManager* manager)
        : ConfigDiffTracker<shared_ptr<Chunk>>(ns, currMap, maxVersion, maxShardVersions),
          _manager(manager) {}

    bool isTracked(const ChunkType& chunk) const final {
        // Mongos tracks all shards
        return true;
    }

    bool isMinKeyIndexed() const final {
        return false;
    }

    pair<BSONObj, shared_ptr<Chunk>> rangeFor(OperationContext* txn,
                                              const ChunkType& chunk) const final {
        return std::make_pair(chunk.getMax(), std::make_shared<Chunk>(chunk));
    }

    ShardId shardFor(OperationContext* txn, const ShardId& shardId) const final {
        const auto shard = uassertStatusOK(Grid::get(txn)->shardRegistry()->getShard(txn, shardId));
        return shard->getId();
    }

private:
    ChunkManager* const _manager;
};

bool allOfType(BSONType type, const BSONObj& o) {
    BSONObjIterator it(o);
    while (it.more()) {
        if (it.next().type() != type) {
            return false;
        }
    }
    return true;
}

bool isChunkMapValid(const ChunkMap& chunkMap) {
#define ENSURE(x)                                          \
    do {                                                   \
        if (!(x)) {                                        \
            log() << "ChunkManager::_isValid failed: " #x; \
            return false;                                  \
        }                                                  \
    } while (0)

    if (chunkMap.empty()) {
        return true;
    }

    // Check endpoints
    ENSURE(allOfType(MinKey, chunkMap.begin()->second->getMin()));
    ENSURE(allOfType(MaxKey, boost::prior(chunkMap.end())->second->getMax()));

    // Make sure there are no gaps or overlaps
    for (ChunkMap::const_iterator it = boost::next(chunkMap.begin()), end = chunkMap.end();
         it != end;
         ++it) {
        ChunkMap::const_iterator last = boost::prior(it);

        if (SimpleBSONObjComparator::kInstance.evaluate(it->second->getMin() !=
                                                        last->second->getMax())) {
            log() << last->second->toString();
            log() << it->second->toString();
            log() << it->second->getMin();
            log() << last->second->getMax();
        }

        ENSURE(SimpleBSONObjComparator::kInstance.evaluate(it->second->getMin() ==
                                                           last->second->getMax()));
    }

    return true;

#undef ENSURE
}

}  // namespace

ChunkManager::ChunkManager(NamespaceString nss,
                           const OID& epoch,
                           const ShardKeyPattern& shardKeyPattern,
                           std::unique_ptr<CollatorInterface> defaultCollator,
                           bool unique)
    : _sequenceNumber(nextCMSequenceNumber.addAndFetch(1)),
      _nss(std::move(nss)),
      _keyPattern(shardKeyPattern.getKeyPattern()),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _chunkMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<std::shared_ptr<Chunk>>()),
      _chunkRangeMap(
          SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<ShardAndChunkRange>()),
      _version(0, 0, epoch) {}

ChunkManager::~ChunkManager() = default;

void ChunkManager::loadExistingRanges(OperationContext* txn, const ChunkManager* oldManager) {
    invariant(!_version.isSet());

    int tries = 3;

    while (tries--) {
        ChunkMap chunkMap =
            SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<std::shared_ptr<Chunk>>();
        set<ShardId> shardIds;
        ShardVersionMap shardVersions;

        Timer t;

        log() << "ChunkManager loading chunks for " << _nss
              << " sequenceNumber: " << _sequenceNumber
              << " based on: " << (oldManager ? oldManager->getVersion().toString() : "(empty)");

        if (_load(txn, chunkMap, shardIds, &shardVersions, oldManager)) {
            // TODO: Merge into diff code above, so we validate in one place
            if (isChunkMapValid(chunkMap)) {
                _chunkMap = std::move(chunkMap);
                _shardVersions = std::move(shardVersions);
                _chunkRangeMap = _constructRanges(_chunkMap);

                log() << "ChunkManager load took " << t.millis() << " ms and found version "
                      << _version;

                return;
            }
        }

        warning() << "ChunkManager load failed after " << t.millis()
                  << " ms and will be retried up to " << tries << " more times";

        sleepmillis(10 * (3 - tries));
    }

    // This will abort construction so we should never have a reference to an invalid config
    msgasserted(13282,
                str::stream() << "Couldn't load a valid config for " << _nss.ns()
                              << " after 3 attempts. Please try again.");
}

bool ChunkManager::_load(OperationContext* txn,
                         ChunkMap& chunkMap,
                         set<ShardId>& shardIds,
                         ShardVersionMap* shardVersions,
                         const ChunkManager* oldManager) {
    // Reset the max version, but not the epoch, when we aren't loading from the oldManager
    _version = ChunkVersion(0, 0, _version.epoch());

    // If we have a previous version of the ChunkManager to work from, use that info to reduce
    // our config query
    if (oldManager && oldManager->getVersion().isSet()) {
        // Get the old max version
        _version = oldManager->getVersion();

        // Load a copy of the old versions
        *shardVersions = oldManager->_shardVersions;

        // Load a copy of the chunk map, replacing the chunk manager with our own
        const ChunkMap& oldChunkMap = oldManager->getChunkMap();

        for (const auto& oldChunkMapEntry : oldChunkMap) {
            const auto& oldC = oldChunkMapEntry.second;
            chunkMap.emplace(oldC->getMax(), std::make_shared<Chunk>(*oldC));
        }

        LOG(2) << "loading chunk manager for collection " << _nss
               << " using old chunk manager w/ version " << _version.toString() << " and "
               << oldChunkMap.size() << " chunks";
    }

    // Attach a diff tracker for the versioned chunk data
    CMConfigDiffTracker differ(_nss.ns(), &chunkMap, &_version, shardVersions, this);

    // Diff tracker should *always* find at least one chunk if collection exists
    // Get the diff query required
    auto diffQuery = differ.configDiffQuery();

    repl::OpTime opTime;
    std::vector<ChunkType> chunks;
    uassertStatusOK(Grid::get(txn)->catalogClient(txn)->getChunks(
        txn,
        diffQuery.query,
        diffQuery.sort,
        boost::none,
        &chunks,
        &opTime,
        repl::ReadConcernLevel::kMajorityReadConcern));

    invariant(opTime >= _configOpTime);
    _configOpTime = opTime;

    int diffsApplied = differ.calculateConfigDiff(txn, chunks);
    if (diffsApplied > 0) {
        LOG(2) << "loaded " << diffsApplied << " chunks into new chunk manager for " << _nss
               << " with version " << _version;

        // Add all existing shards we find to the shards set
        for (ShardVersionMap::iterator it = shardVersions->begin(); it != shardVersions->end();) {
            auto shardStatus = Grid::get(txn)->shardRegistry()->getShard(txn, it->first);
            if (shardStatus.isOK()) {
                shardIds.insert(it->first);
                ++it;
            } else {
                invariant(shardStatus == ErrorCodes::ShardNotFound);
                shardVersions->erase(it++);
            }
        }

        _configOpTime = opTime;

        return true;
    } else if (diffsApplied == 0) {
        // No chunks were found for the ns
        warning() << "no chunks found when reloading " << _nss << ", previous version was "
                  << _version;

        // Set all our data to empty
        chunkMap.clear();
        shardVersions->clear();

        _version = ChunkVersion(0, 0, OID());
        _configOpTime = opTime;

        return true;
    } else {  // diffsApplied < 0

        bool allInconsistent = (differ.numValidDiffs() == 0);
        if (allInconsistent) {
            // All versions are different, this can be normal
            warning() << "major change in chunk information found when reloading " << _nss
                      << ", previous version was " << _version;
        } else {
            // Inconsistent load halfway through (due to yielding cursor during load)
            // should be rare
            warning() << "inconsistent chunks found when reloading " << _nss
                      << ", previous version was " << _version << ", this should be rare";
        }

        // Set all our data to empty to be extra safe
        chunkMap.clear();
        shardVersions->clear();

        _version = ChunkVersion(0, 0, OID());

        return allInconsistent;
    }
}

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
                                       set<ShardId>* shardIds) const {
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
    auto shardKeyToFind = _keyPattern.extractShardKeyFromQuery(*cq);
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
    IndexBounds bounds = getIndexBoundsForQuery(_keyPattern.toBSON(), *cq);

    // Transforms bounds for each shard key field into full shard key ranges
    // for example :
    //   Key { a : 1, b : 1 }
    //   Bounds { a : [1, 2), b : [3, 4) }
    //   => Ranges { a : 1, b : 3 } => { a : 2, b : 4 }
    BoundList ranges = _keyPattern.flattenBounds(bounds);

    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        getShardIdsForRange(it->first /*min*/, it->second /*max*/, shardIds);

        // once we know we need to visit all shards no need to keep looping
        if (shardIds->size() == _shardVersions.size()) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        shardIds->insert(_chunkRangeMap.begin()->second.getShardId());
    }
}

void ChunkManager::getShardIdsForRange(const BSONObj& min,
                                       const BSONObj& max,
                                       std::set<ShardId>* shardIds) const {
    auto it = _chunkRangeMap.upper_bound(min);
    auto end = _chunkRangeMap.upper_bound(max);

    // The chunk range map must always cover the entire key space
    invariant(it != _chunkRangeMap.end());

    // We need to include the last chunk
    if (end != _chunkRangeMap.cend()) {
        ++end;
    }

    for (; it != end; ++it) {
        shardIds->insert(it->second.getShardId());

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards.
        if (shardIds->size() == _shardVersions.size()) {
            break;
        }
    }
}

void ChunkManager::getAllShardIds(set<ShardId>* all) const {
    std::transform(_shardVersions.begin(),
                   _shardVersions.end(),
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
    string accessMethod = IndexNames::findPluginName(key);
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
    auto it = _shardVersions.find(shardName);
    if (it == _shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch)
        return ChunkVersion(0, 0, _version.epoch());
    }

    return it->second;
}

string ChunkManager::toString() const {
    StringBuilder sb;
    sb << "ChunkManager: " << _nss.ns() << " key:" << _keyPattern.toString() << '\n';

    for (const auto& entry : _chunkMap) {
        sb << "\t" << entry.second->toString() << '\n';
    }

    return sb.str();
}

ChunkManager::ChunkRangeMap ChunkManager::_constructRanges(const ChunkMap& chunkMap) {
    ChunkRangeMap chunkRangeMap =
        SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<ShardAndChunkRange>();

    if (chunkMap.empty()) {
        return chunkRangeMap;
    }

    ChunkMap::const_iterator current = chunkMap.cbegin();

    while (current != chunkMap.cend()) {
        const auto rangeFirst = current;
        current = std::find_if(
            current, chunkMap.cend(), [&rangeFirst](const ChunkMap::value_type& chunkMapEntry) {
                return chunkMapEntry.second->getShardId() != rangeFirst->second->getShardId();
            });
        const auto rangeLast = std::prev(current);

        const BSONObj rangeMin = rangeFirst->second->getMin();
        const BSONObj rangeMax = rangeLast->second->getMax();

        auto insertResult = chunkRangeMap.insert(std::make_pair(
            rangeMax, ShardAndChunkRange(rangeMin, rangeMax, rangeFirst->second->getShardId())));
        invariant(insertResult.second);
        if (insertResult.first != chunkRangeMap.begin()) {
            // Make sure there are no gaps in the ranges
            insertResult.first--;
            invariant(
                SimpleBSONObjComparator::kInstance.evaluate(insertResult.first->first == rangeMin));
        }
    }

    invariant(!chunkRangeMap.empty());
    invariant(allOfType(MinKey, chunkRangeMap.begin()->second.getMin()));
    invariant(allOfType(MaxKey, chunkRangeMap.rbegin()->first));

    return chunkRangeMap;
}

repl::OpTime ChunkManager::getConfigOpTime() const {
    return _configOpTime;
}

}  // namespace mongo
