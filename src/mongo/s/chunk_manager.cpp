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
#include <map>
#include <set>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::make_pair;
using std::map;
using std::max;
using std::pair;
using std::set;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {

/**
 * This is an adapter so we can use config diffs - mongos and mongod do them slightly
 * differently
 *
 * The mongos adapter here tracks all shards, and stores ranges by (max, Chunk) in the map.
 */
class CMConfigDiffTracker : public ConfigDiffTracker<shared_ptr<Chunk>> {
public:
    CMConfigDiffTracker(ChunkManager* manager) : _manager(manager) {}

    bool isTracked(const ChunkType& chunk) const final {
        // Mongos tracks all shards
        return true;
    }

    bool isMinKeyIndexed() const final {
        return false;
    }

    pair<BSONObj, shared_ptr<Chunk>> rangeFor(OperationContext* txn,
                                              const ChunkType& chunk) const final {
        shared_ptr<Chunk> c(new Chunk(txn, _manager, chunk));
        return make_pair(chunk.getMax(), c);
    }

    string shardFor(OperationContext* txn, const string& hostName) const final {
        const auto shard = grid.shardRegistry()->getShard(txn, hostName);
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

        if (!(it->second->getMin() == last->second->getMax())) {
            log() << last->second->toString();
            log() << it->second->toString();
            log() << it->second->getMin();
            log() << last->second->getMax();
        }

        ENSURE(it->second->getMin() == last->second->getMax());
    }

    return true;

#undef ENSURE
}

}  // namespace

AtomicUInt32 ChunkManager::NextSequenceNumber(1U);

ChunkManager::ChunkManager(const string& ns, const ShardKeyPattern& pattern, bool unique)
    : _ns(ns),
      _keyPattern(pattern.getKeyPattern()),
      _unique(unique),
      _sequenceNumber(NextSequenceNumber.addAndFetch(1)),
      _chunkRanges() {}

ChunkManager::ChunkManager(const CollectionType& coll)
    : _ns(coll.getNs().ns()),
      _keyPattern(coll.getKeyPattern()),
      _unique(coll.getUnique()),
      _sequenceNumber(NextSequenceNumber.addAndFetch(1)),
      _chunkRanges() {
    // coll does not have correct version. Use same initial version as _load and createFirstChunks.
    _version = ChunkVersion(0, 0, coll.getEpoch());
}

void ChunkManager::loadExistingRanges(OperationContext* txn, const ChunkManager* oldManager) {
    int tries = 3;

    while (tries--) {
        ChunkMap chunkMap;
        set<ShardId> shardIds;
        ShardVersionMap shardVersions;

        Timer t;

        bool success = _load(txn, chunkMap, shardIds, &shardVersions, oldManager);
        if (success) {
            log() << "ChunkManager: time to load chunks for " << _ns << ": " << t.millis() << "ms"
                  << " sequenceNumber: " << _sequenceNumber << " version: " << _version.toString()
                  << " based on: "
                  << (oldManager ? oldManager->getVersion().toString() : "(empty)");

            // TODO: Merge into diff code above, so we validate in one place
            if (isChunkMapValid(chunkMap)) {
                _chunkMap.swap(chunkMap);
                _shardIds.swap(shardIds);
                _shardVersions.swap(shardVersions);
                _chunkRanges.reloadAll(_chunkMap);

                return;
            }
        }

        if (_chunkMap.size() < 10) {
            _printChunks();
        }

        warning() << "ChunkManager loaded an invalid config for " << _ns << ", trying again";

        sleepmillis(10 * (3 - tries));
    }

    // This will abort construction so we should never have a reference to an invalid config
    msgasserted(13282,
                str::stream() << "Couldn't load a valid config for " << _ns
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

        // Could be v.expensive
        // TODO: If chunks were immutable and didn't reference the manager, we could do more
        // interesting things here
        for (const auto& oldChunkMapEntry : oldChunkMap) {
            shared_ptr<Chunk> oldC = oldChunkMapEntry.second;
            shared_ptr<Chunk> newC(new Chunk(
                this, oldC->getMin(), oldC->getMax(), oldC->getShardId(), oldC->getLastmod()));

            newC->setBytesWritten(oldC->getBytesWritten());

            chunkMap.insert(make_pair(oldC->getMax(), newC));
        }

        LOG(2) << "loading chunk manager for collection " << _ns
               << " using old chunk manager w/ version " << _version.toString() << " and "
               << oldChunkMap.size() << " chunks";
    }

    // Attach a diff tracker for the versioned chunk data
    CMConfigDiffTracker differ(this);
    differ.attach(_ns, chunkMap, _version, *shardVersions);

    // Diff tracker should *always* find at least one chunk if collection exists
    // Get the diff query required
    auto diffQuery = differ.configDiffQuery();

    repl::OpTime opTime;
    std::vector<ChunkType> chunks;
    uassertStatusOK(grid.catalogManager(txn)->getChunks(
        txn, diffQuery.query, diffQuery.sort, boost::none, &chunks, &opTime));

    invariant(opTime >= _configOpTime);
    _configOpTime = opTime;

    int diffsApplied = differ.calculateConfigDiff(txn, chunks);
    if (diffsApplied > 0) {
        LOG(2) << "loaded " << diffsApplied << " chunks into new chunk manager for " << _ns
               << " with version " << _version;

        // Add all existing shards we find to the shards set
        for (ShardVersionMap::iterator it = shardVersions->begin(); it != shardVersions->end();) {
            shared_ptr<Shard> shard = grid.shardRegistry()->getShard(txn, it->first);
            if (shard) {
                shardIds.insert(it->first);
                ++it;
            } else {
                shardVersions->erase(it++);
            }
        }

        _configOpTime = opTime;

        return true;
    } else if (diffsApplied == 0) {
        // No chunks were found for the ns
        warning() << "no chunks found when reloading " << _ns << ", previous version was "
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
            warning() << "major change in chunk information found when reloading " << _ns
                      << ", previous version was " << _version;
        } else {
            // Inconsistent load halfway through (due to yielding cursor during load)
            // should be rare
            warning() << "inconsistent chunks found when reloading " << _ns
                      << ", previous version was " << _version << ", this should be rare";
        }

        // Set all our data to empty to be extra safe
        chunkMap.clear();
        shardVersions->clear();

        _version = ChunkVersion(0, 0, OID());

        return allInconsistent;
    }
}

shared_ptr<ChunkManager> ChunkManager::reload(OperationContext* txn, bool force) const {
    const NamespaceString nss(_ns);
    auto config = uassertStatusOK(grid.catalogCache()->getDatabase(txn, nss.db().toString()));

    return config->getChunkManagerIfExists(txn, getns(), force);
}

void ChunkManager::_printChunks() const {
    for (ChunkMap::const_iterator it = _chunkMap.begin(), end = _chunkMap.end(); it != end; ++it) {
        log() << *it->second;
    }
}

void ChunkManager::calcInitSplitsAndShards(OperationContext* txn,
                                           const ShardId& primaryShardId,
                                           const vector<BSONObj>* initPoints,
                                           const set<ShardId>* initShardIds,
                                           vector<BSONObj>* splitPoints,
                                           vector<ShardId>* shardIds) const {
    verify(_chunkMap.size() == 0);

    Chunk c(this,
            _keyPattern.getKeyPattern().globalMin(),
            _keyPattern.getKeyPattern().globalMax(),
            primaryShardId);

    if (!initPoints || !initPoints->size()) {
        // discover split points
        const auto primaryShard = grid.shardRegistry()->getShard(txn, primaryShardId);
        const NamespaceString nss{getns()};
        auto result = grid.shardRegistry()->runCommandOnShard(
            txn,
            primaryShard,
            ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
            nss.db().toString(),
            BSON("count" << nss.coll()));

        long long numObjects = 0;
        uassertStatusOK(result.getStatus());
        uassertStatusOK(Command::getStatusFromCommandResult(result.getValue()));
        uassertStatusOK(bsonExtractIntegerField(result.getValue(), "n", &numObjects));

        if (numObjects > 0)
            c.pickSplitVector(txn, *splitPoints, Chunk::MaxChunkSize);

        // since docs already exists, must use primary shard
        shardIds->push_back(primaryShardId);
    } else {
        // make sure points are unique and ordered
        set<BSONObj> orderedPts;
        for (unsigned i = 0; i < initPoints->size(); ++i) {
            BSONObj pt = (*initPoints)[i];
            orderedPts.insert(pt);
        }
        for (set<BSONObj>::iterator it = orderedPts.begin(); it != orderedPts.end(); ++it) {
            splitPoints->push_back(*it);
        }

        if (!initShardIds || !initShardIds->size()) {
            // If not specified, only use the primary shard (note that it's not safe for mongos
            // to put initial chunks on other shards without the primary mongod knowing).
            shardIds->push_back(primaryShardId);
        } else {
            std::copy(initShardIds->begin(), initShardIds->end(), std::back_inserter(*shardIds));
        }
    }
}

Status ChunkManager::createFirstChunks(OperationContext* txn,
                                       const ShardId& primaryShardId,
                                       const vector<BSONObj>* initPoints,
                                       const set<ShardId>* initShardIds) {
    // TODO distlock?
    // TODO: Race condition if we shard the collection and insert data while we split across
    // the non-primary shard.

    vector<BSONObj> splitPoints;
    vector<ShardId> shardIds;
    calcInitSplitsAndShards(txn, primaryShardId, initPoints, initShardIds, &splitPoints, &shardIds);

    // this is the first chunk; start the versioning from scratch
    ChunkVersion version(1, 0, OID::gen());

    log() << "going to create " << splitPoints.size() + 1 << " chunk(s) for: " << _ns
          << " using new epoch " << version.epoch();

    for (unsigned i = 0; i <= splitPoints.size(); i++) {
        BSONObj min = i == 0 ? _keyPattern.getKeyPattern().globalMin() : splitPoints[i - 1];
        BSONObj max =
            i < splitPoints.size() ? splitPoints[i] : _keyPattern.getKeyPattern().globalMax();

        ChunkType chunk;
        chunk.setName(Chunk::genID(_ns, min));
        chunk.setNS(_ns);
        chunk.setMin(min);
        chunk.setMax(max);
        chunk.setShard(shardIds[i % shardIds.size()]);
        chunk.setVersion(version);

        Status status = grid.catalogManager(txn)
                            ->insertConfigDocument(txn, ChunkType::ConfigNS, chunk.toBSON());
        if (!status.isOK()) {
            const string errMsg = str::stream()
                << "Creating first chunks failed: " << status.reason();
            error() << errMsg;
            return Status(status.code(), errMsg);
        }

        version.incMinor();
    }

    _version = ChunkVersion(0, 0, version.epoch());

    return Status::OK();
}

ChunkPtr ChunkManager::findIntersectingChunk(OperationContext* txn, const BSONObj& shardKey) const {
    {
        BSONObj chunkMin;
        ChunkPtr chunk;
        {
            ChunkMap::const_iterator it = _chunkMap.upper_bound(shardKey);
            if (it != _chunkMap.end()) {
                chunkMin = it->first;
                chunk = it->second;
            }
        }

        if (chunk) {
            if (chunk->containsKey(shardKey)) {
                return chunk;
            }

            log() << chunkMin;
            log() << *chunk;
            log() << shardKey;

            reload(txn);
            msgasserted(13141, "Chunk map pointed to incorrect chunk");
        }
    }

    msgasserted(8070,
                str::stream() << "couldn't find a chunk intersecting: " << shardKey
                              << " for ns: " << _ns << " at version: " << _version.toString()
                              << ", number of chunks: " << _chunkMap.size());
}

void ChunkManager::getShardIdsForQuery(OperationContext* txn,
                                       const BSONObj& query,
                                       set<ShardId>* shardIds) const {
    auto statusWithCQ =
        CanonicalQuery::canonicalize(NamespaceString(_ns), query, ExtensionsCallbackNoop());

    uassertStatusOK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Query validation
    if (QueryPlannerCommon::hasNode(cq->root(), MatchExpression::GEO_NEAR)) {
        uassert(13501, "use geoNear command rather than $near query", false);
    }

    // Fast path for targeting equalities on the shard key.
    auto shardKeyToFind = _keyPattern.extractShardKeyFromQuery(*cq);
    if (shardKeyToFind.isOK() && !shardKeyToFind.getValue().isEmpty()) {
        auto chunk = findIntersectingChunk(txn, shardKeyToFind.getValue());
        shardIds->insert(chunk->getShardId());
        return;
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
        getShardIdsForRange(*shardIds, it->first /*min*/, it->second /*max*/);

        // once we know we need to visit all shards no need to keep looping
        if (shardIds->size() == _shardIds.size())
            break;
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be
    // returned.  For now, we satisfy that assumption by adding a shard with no matches rather
    // than return an empty set of shards.
    if (shardIds->empty()) {
        massert(16068, "no chunk ranges available", !_chunkRanges.ranges().empty());
        shardIds->insert(_chunkRanges.ranges().begin()->second->getShardId());
    }
}

void ChunkManager::getShardIdsForRange(set<ShardId>& shardIds,
                                       const BSONObj& min,
                                       const BSONObj& max) const {
    ChunkRangeMap::const_iterator it = _chunkRanges.upper_bound(min);
    ChunkRangeMap::const_iterator end = _chunkRanges.upper_bound(max);

    massert(13507,
            str::stream() << "no chunks found between bounds " << min << " and " << max,
            it != _chunkRanges.ranges().end());

    if (end != _chunkRanges.ranges().end())
        ++end;

    for (; it != end; ++it) {
        shardIds.insert(it->second->getShardId());

        // once we know we need to visit all shards no need to keep looping
        if (shardIds.size() == _shardIds.size())
            break;
    }
}

void ChunkManager::getAllShardIds(set<ShardId>* all) const {
    dassert(all);

    all->insert(_shardIds.begin(), _shardIds.end());
}

IndexBounds ChunkManager::getIndexBoundsForQuery(const BSONObj& key,
                                                 const CanonicalQuery& canonicalQuery) {
    // TODO: special-casing TEXT here is no longer necessary.  The work to remove this special case
    // is being tracked at SERVER-21511.
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
                          false /* sparse */,
                          false /* unique */,
                          "shardkey",
                          NULL /* filterExpr */,
                          BSONObj());
    plannerParams.indices.push_back(indexEntry);

    OwnedPointerVector<QuerySolution> solutions;
    Status status = QueryPlanner::plan(canonicalQuery, plannerParams, &solutions.mutableVector());
    uassert(status.code(), status.reason(), status.isOK());

    IndexBounds bounds;

    for (vector<QuerySolution*>::const_iterator it = solutions.begin();
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
    if (node->children.size() == 0) {
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
        error() << "could not generate index bounds on query solution tree: " << node->toString();
        dassert(false);  // We'd like to know this error in testing.

        // Bail out with all shards in production, since this isn't a fatal error.
        return IndexBounds();
    }

    IndexBounds bounds;
    for (vector<QuerySolutionNode*>::const_iterator it = node->children.begin();
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
        if (childBounds.size() == 0) {  // Got unexpected node in query solution tree
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

bool ChunkManager::compatibleWith(const ChunkManager& other, const string& shardName) const {
    // Return true if the shard version is the same in the two chunk managers
    // TODO: This doesn't need to be so strong, just major vs
    return other.getVersion(shardName).equals(getVersion(shardName));
}

ChunkVersion ChunkManager::getVersion(const std::string& shardName) const {
    ShardVersionMap::const_iterator i = _shardVersions.find(shardName);
    if (i == _shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have
        // no chunks) always have a version of (0, 0, epoch).  Note this is
        // *different* from the dropped chunk version of (0, 0, OID(000...)).
        // See s/chunk_version.h.
        return ChunkVersion(0, 0, _version.epoch());
    }
    return i->second;
}

ChunkVersion ChunkManager::getVersion() const {
    return _version;
}

string ChunkManager::toString() const {
    StringBuilder sb;
    sb << "ChunkManager: " << _ns << " key:" << _keyPattern.toString() << '\n';

    for (ChunkMap::const_iterator i = _chunkMap.begin(); i != _chunkMap.end(); ++i) {
        sb << "\t" << i->second->toString() << '\n';
    }

    return sb.str();
}


ChunkRange::ChunkRange(ChunkMap::const_iterator begin, const ChunkMap::const_iterator end)
    : _manager(begin->second->getManager()),
      _shardId(begin->second->getShardId()),
      _min(begin->second->getMin()),
      _max(boost::prior(end)->second->getMax()) {
    invariant(begin != end);

    DEV while (begin != end) {
        dassert(begin->second->getManager() == _manager);
        dassert(begin->second->getShardId() == _shardId);
        ++begin;
    }
}

ChunkRange::ChunkRange(const ChunkRange& min, const ChunkRange& max)
    : _manager(min.getManager()),
      _shardId(min.getShardId()),
      _min(min.getMin()),
      _max(max.getMax()) {
    invariant(min.getShardId() == max.getShardId());
    invariant(min.getManager() == max.getManager());
    invariant(min.getMax() == max.getMin());
}

string ChunkRange::toString() const {
    StringBuilder sb;
    sb << "ChunkRange(min=" << _min << ", max=" << _max << ", shard=" << _shardId << ")";

    return sb.str();
}


void ChunkRangeManager::assertValid() const {
    if (_ranges.empty())
        return;

    try {
        // No Nulls
        for (ChunkRangeMap::const_iterator it = _ranges.begin(), end = _ranges.end(); it != end;
             ++it) {
            verify(it->second);
        }

        // Check endpoints
        verify(allOfType(MinKey, _ranges.begin()->second->getMin()));
        verify(allOfType(MaxKey, boost::prior(_ranges.end())->second->getMax()));

        // Make sure there are no gaps or overlaps
        for (ChunkRangeMap::const_iterator it = boost::next(_ranges.begin()), end = _ranges.end();
             it != end;
             ++it) {
            ChunkRangeMap::const_iterator last = boost::prior(it);
            verify(it->second->getMin() == last->second->getMax());
        }

        // Check Map keys
        for (ChunkRangeMap::const_iterator it = _ranges.begin(), end = _ranges.end(); it != end;
             ++it) {
            verify(it->first == it->second->getMax());
        }

        // Make sure we match the original chunks
        const ChunkMap chunks = _ranges.begin()->second->getManager()->_chunkMap;
        for (ChunkMap::const_iterator i = chunks.begin(); i != chunks.end(); ++i) {
            const ChunkPtr chunk = i->second;

            ChunkRangeMap::const_iterator min = _ranges.upper_bound(chunk->getMin());
            ChunkRangeMap::const_iterator max = _ranges.lower_bound(chunk->getMax());

            verify(min != _ranges.end());
            verify(max != _ranges.end());
            verify(min == max);
            verify(min->second->getShardId() == chunk->getShardId());
            verify(min->second->containsKey(chunk->getMin()));
            verify(min->second->containsKey(chunk->getMax()) ||
                   (min->second->getMax() == chunk->getMax()));
        }

    } catch (...) {
        error() << "\t invalid ChunkRangeMap! printing ranges:";

        for (ChunkRangeMap::const_iterator it = _ranges.begin(), end = _ranges.end(); it != end;
             ++it) {
            log() << it->first << ": " << it->second->toString();
        }

        throw;
    }
}

void ChunkRangeManager::reloadAll(const ChunkMap& chunks) {
    _ranges.clear();
    _insertRange(chunks.begin(), chunks.end());

    DEV assertValid();
}

void ChunkRangeManager::_insertRange(ChunkMap::const_iterator begin,
                                     const ChunkMap::const_iterator end) {
    while (begin != end) {
        ChunkMap::const_iterator first = begin;
        ShardId shardId = first->second->getShardId();
        while (begin != end && (begin->second->getShardId() == shardId))
            ++begin;

        shared_ptr<ChunkRange> cr(new ChunkRange(first, begin));
        _ranges[cr->getMax()] = cr;
    }
}

int ChunkManager::getCurrentDesiredChunkSize() const {
    // split faster in early chunks helps spread out an initial load better
    const int minChunkSize = 1 << 20;  // 1 MBytes

    int splitThreshold = Chunk::MaxChunkSize;

    int nc = numChunks();

    if (nc <= 1) {
        return 1024;
    } else if (nc < 3) {
        return minChunkSize / 2;
    } else if (nc < 10) {
        splitThreshold = max(splitThreshold / 4, minChunkSize);
    } else if (nc < 20) {
        splitThreshold = max(splitThreshold / 2, minChunkSize);
    }

    return splitThreshold;
}

repl::OpTime ChunkManager::getConfigOpTime() const {
    return _configOpTime;
}

}  // namespace mongo
