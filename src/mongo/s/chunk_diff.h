/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class ChunkType;
struct ChunkVersion;
class OperationContext;

class ConfigDiffTrackerBase {
public:
    /**
     * Structure repsenting the generated query and sort order for a chunk diffing operation.
     */
    struct QueryAndSort {
        QueryAndSort(BSONObj inQuery, BSONObj inSort) : query(inQuery), sort(inSort) {}

        std::string toString() const;

        const BSONObj query;
        const BSONObj sort;
    };
};

/**
 * This class manages and applies diffs from partial config server data reloads. Because the config
 * data can be large, we want to update it in small parts, not all-at-once. Once a
 * ConfigDiffTracker is created, the current config data is *attached* to it, and it is then able
 * to modify the data.
 *
 * The current form is templated b/c the overall algorithm is identical between mongos and mongod,
 * but the actual chunk maps used differ in implementation. We don't want to copy the
 * implementation, because the logic is identical, or the chunk data, because that would be slow
 * for big clusters, so this is the alternative for now.
 *
 * TODO: Standardize between mongos and mongod and convert template parameters to types.
 */
template <class ValType>
class ConfigDiffTracker : public ConfigDiffTrackerBase {
public:
    // Stores ranges indexed by max or  min key
    typedef typename std::map<BSONObj, ValType, BSONObjCmp> RangeMap;

    // Pair of iterators defining a subset of ranges
    typedef typename std::pair<typename RangeMap::iterator, typename RangeMap::iterator>
        RangeOverlap;

    // Map of shard identifiers to the maximum chunk version on that shard
    typedef typename std::map<ShardId, ChunkVersion> MaxChunkVersionMap;

    ConfigDiffTracker();
    virtual ~ConfigDiffTracker();

    /**
     * The tracker attaches to a set of ranges with versions, and uses a config server
     * connection to update these. Because the set of ranges and versions may be large, they
     * aren't owned by the tracker, they're just passed in and updated.  Therefore they must all
     * stay in scope while the tracker is working.
     *
     * TODO: Make a standard VersionedRange to encapsulate this info in both mongod and mongos?
     */
    void attach(const std::string& ns,
                RangeMap& currMap,
                ChunkVersion& maxVersion,
                MaxChunkVersionMap& maxShardVersions);

    // Call after load for more information
    int numValidDiffs() const {
        return _validDiffs;
    }

    // Applies changes to the config data from a vector of chunks passed in. Also includes minor
    // version changes for particular major-version chunks if explicitly specified.
    // Returns the number of diffs processed, or -1 if the diffs were inconsistent.
    int calculateConfigDiff(OperationContext* txn, const std::vector<ChunkType>& chunks);

    // Returns the query needed to find new changes to a collection from the config server
    // Needed only if a custom connection is required to the config server
    QueryAndSort configDiffQuery() const;

protected:
    /**
     * Determines which chunks are actually being remembered by our RangeMap. Allows individual
     * shards to filter out results, which belong to the local shard only.
     */
    virtual bool isTracked(const ChunkType& chunk) const = 0;

    /**
     * Whether or not our RangeMap uses min or max keys
     */
    virtual bool isMinKeyIndexed() const {
        return true;
    }

    virtual std::pair<BSONObj, ValType> rangeFor(OperationContext* txn,
                                                 const ChunkType& chunk) const = 0;

    virtual ShardId shardFor(OperationContext* txn, const ShardId& name) const = 0;

private:
    void _assertAttached() const;

    // Whether or not a range exists in the min/max region
    bool _isOverlapping(const BSONObj& min, const BSONObj& max);

    // Returns a subset of ranges overlapping the region min/max
    RangeOverlap _overlappingRange(const BSONObj& min, const BSONObj& max);

    std::string _ns;
    RangeMap* _currMap;
    ChunkVersion* _maxVersion;
    MaxChunkVersionMap* _maxShardVersions;

    // Store for later use
    int _validDiffs;
};

}  // namespace mongo
