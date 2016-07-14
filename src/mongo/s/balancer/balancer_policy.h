/**
*    Copyright (C) 2010 10gen Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/s/balancer/cluster_statistics.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class ChunkManager;
class OperationContext;

struct TagRange {
    TagRange() = default;

    TagRange(const BSONObj& a_min, const BSONObj& a_max, const std::string& a_tag)
        : min(a_min.getOwned()), max(a_max.getOwned()), tag(a_tag) {}

    std::string toString() const;

    BSONObj min;
    BSONObj max;
    std::string tag;
};

struct MigrateInfo {
    MigrateInfo(const std::string& a_ns, const ShardId& a_to, const ChunkType& a_chunk)
        : ns(a_ns),
          to(a_to),
          from(a_chunk.getShard()),
          minKey(a_chunk.getMin()),
          maxKey(a_chunk.getMax()) {}

    std::string getName() const;
    std::string toString() const;

    std::string ns;
    ShardId to;
    ShardId from;
    BSONObj minKey;
    BSONObj maxKey;
};

typedef std::vector<ClusterStatistics::ShardStatistics> ShardStatisticsVector;
typedef std::map<ShardId, std::vector<ChunkType>> ShardToChunksMap;

/**
 * This class constitutes a cache of the chunk distribution across the entire cluster along with the
 * zone boundaries imposed on it. This information is stored in format, which makes it efficient to
 * query utilization statististics and to decide what to balance.
 */
class DistributionStatus {
    MONGO_DISALLOW_COPYING(DistributionStatus);

public:
    DistributionStatus(NamespaceString nss, ShardToChunksMap shardToChunksMap);
    DistributionStatus(DistributionStatus&&) = default;

    /**
     * Returns the namespace for which this balance status applies.
     */
    const NamespaceString& nss() const {
        return _nss;
    }

    /**
     * Appends the specified range to the set of ranges tracked for this collection and checks its
     * valididty. Returns true if the range is valid or false otherwise.
     */
    bool addTagRange(const TagRange& range);

    /**
     * Returns total number of chunks across all shards.
     */
    size_t totalChunks() const;

    /**
     * Returns the total number of chunks across all shards, which fall into the specified zone's
     * range.
     */
    size_t totalChunksWithTag(const std::string& tag) const;

    /**
     * Returns number of chunks in the specified shard.
     */
    size_t numberOfChunksInShard(const ShardId& shardId) const;

    /**
     * Returns number of chunks in the specified shard, which have the given tag.
     */
    size_t numberOfChunksInShardWithTag(const ShardId& shardId, const std::string& tag) const;

    /**
     * Returns all chunks for the specified shard.
     */
    const std::vector<ChunkType>& getChunks(const ShardId& shardId) const;

    /**
     * Returns all tag ranges defined for the collection.
     */
    const std::map<BSONObj, TagRange, BSONObjCmp>& tagRanges() const {
        return _tagRanges;
    }

    /**
     * Returns all tags defined for the collection.
     */
    const std::set<std::string>& tags() const {
        return _allTags;
    }

    /**
     * Using the set of tags defined for the collection, returns what tag corresponds to the
     * specified chunk. If the chunk doesn't fall into any tag returns the empty string.
     */
    std::string getTagForChunk(const ChunkType& chunk) const;

    /**
     * Returns a BSON/string representation of this distribution status.
     */
    void report(BSONObjBuilder* builder) const;
    std::string toString() const;

private:
    // Namespace for which this distribution applies
    NamespaceString _nss;

    // Map of what chunks are owned by each shard
    ShardToChunksMap _shardChunks;

    // Map of zone max key to the zone description
    std::map<BSONObj, TagRange, BSONObjCmp> _tagRanges;

    // Set of all zones defined for this collection
    std::set<std::string> _allTags;
};

class BalancerPolicy {
public:
    /**
     * Determines whether a shard with the specified utilization statistics would be able to accept
     * a chunk with the specified tag. According to the policy a shard cannot accept chunks if its
     * size is maxed out and if the chunk's tag conflicts with the tag of the shard.
     */
    static Status isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                          const std::string& chunkTag);

    /**
     * Returns a suggested set of chunks to move whithin a collection's shards, given the specified
     * state of the shards (draining, max size reached, etc) and the number of chunks for that
     * collection. If the policy doesn't recommend anything to move, it returns an empty vector. The
     * entries in the vector do are all for separate source/destination shards and as such do not
     * need to be done serially and can be scheduled in parallel.
     *
     * The balancing logic calculates the optimum number of chunks per shard for each zone and if
     * any of the shards have chunks, which are sufficiently higher than this number, suggests
     * moving chunks to shards, which are under this number.
     *
     * The shouldAggressivelyBalance parameter causes the threshold for chunk could disparity
     * between shards to be lowered.
     */
    static std::vector<MigrateInfo> balance(const ShardStatisticsVector& shardStats,
                                            const DistributionStatus& distribution,
                                            bool shouldAggressivelyBalance);

    /**
     * Using the specified distribution information, returns a suggested better location for the
     * specified chunk if one is available.
     */
    static boost::optional<MigrateInfo> balanceSingleChunk(const ChunkType& chunk,
                                                           const ShardStatisticsVector& shardStats,
                                                           const DistributionStatus& distribution);

private:
    /**
     * Return the shard with the specified tag, which has the least number of chunks. If the tag is
     * empty, considers all shards.
     */
    static ShardId _getLeastLoadedReceiverShard(const ShardStatisticsVector& shardStats,
                                                const DistributionStatus& distribution,
                                                const std::string& tag,
                                                const std::set<ShardId>& excludedShards);

    /**
     * Return the shard which has the least number of chunks with the specified tag. If the tag is
     * empty, considers all chunks.
     */
    static ShardId _getMostOverloadedShard(const ShardStatisticsVector& shardStats,
                                           const DistributionStatus& distribution,
                                           const std::string& chunkTag,
                                           const std::set<ShardId>& excludedShards);

    /**
     * Selects one chunk for the specified zone (if appropriate) to be moved in order to bring the
     * deviation of the shards chunk contents closer to even across all shards in the specified
     * zone. Takes into account the shards, which have already been used for migrations.
     *
     * Returns true if a migration was suggested, false otherwise. This method is intented to be
     * called multiple times until all posible migrations for a zone have been selected.
     */
    static bool _singleZoneBalance(const ShardStatisticsVector& shardStats,
                                   const DistributionStatus& distribution,
                                   const std::string& tag,
                                   size_t imbalanceThreshold,
                                   std::vector<MigrateInfo>* migrations,
                                   std::set<ShardId>* usedShards);
};

}  // namespace mongo
