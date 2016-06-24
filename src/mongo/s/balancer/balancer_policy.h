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
#include "mongo/db/jsobj.h"
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

class DistributionStatus {
    MONGO_DISALLOW_COPYING(DistributionStatus);

public:
    DistributionStatus(ShardStatisticsVector shardInfo, const ShardToChunksMap& shardToChunksMap);

    /**
     * @return if range is valid
     */
    bool addTagRange(const TagRange& range);

    /**
     * Determines whether a shard with the specified utilization statistics would be able to accept
     * a chunk with the specified tag. According to the policy a shard cannot accept chunks if its
     * size is maxed out and if the chunk's tag conflicts with the tag of the shard.
     */
    static Status isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                          const std::string& chunkTag);

    /**
     * @param forTag "" if you don't care, or a tag
     * @return shard best suited to receive a chunk
     */
    ShardId getBestReceieverShard(const std::string& tag) const;

    /**
     * @return the shard with the most chunks
     *         based on # of chunks with the given tag
     */
    ShardId getMostOverloadedShard(const std::string& forTag) const;

    /** @return total number of chunks  */
    unsigned totalChunks() const;

    /** @return number of chunks in this shard */
    unsigned numberOfChunksInShard(const ShardId& shardId) const;

    /** @return number of chunks in this shard with the given tag */
    unsigned numberOfChunksInShardWithTag(const ShardId& shardId, const std::string& tag) const;

    /** @return chunks for the shard */
    const std::vector<ChunkType>& getChunks(const ShardId& shardId) const;

    /** @return all tags we know about, not include "" */
    const std::set<std::string>& tags() const {
        return _allTags;
    }

    /** @return the right tag for chunk, possibly "" */
    std::string getTagForChunk(const ChunkType& chunk) const;

    const ShardStatisticsVector& getStats() const {
        return _shardInfo;
    }

private:
    const ShardStatisticsVector _shardInfo;
    const ShardToChunksMap& _shardChunks;
    std::map<BSONObj, TagRange> _tagRanges;
    std::set<std::string> _allTags;
};

class BalancerPolicy {
public:
    /**
     * Returns a suggested set of chunks to move whithin a collection's shards, given information
     * about space usage and number of chunks for that collection. If the policy doesn't recommend
     * moving, it returns an empty vector.
     *
     * ns is the collection which needs balancing.
     * distribution holds all the info about the current state of the cluster/namespace.
     * shouldAggressivelyBalance indicates that the last round successfully moved chunks around and
     * causes the threshold for chunk number disparity between shards to be lowered.
     *
     * Returns vector of MigrateInfos of the best moves to make towards balacing the specified
     * collection. The entries in the vector do not need to be done serially and can be scheduled in
     * parallel.
     */
    static std::vector<MigrateInfo> balance(const std::string& ns,
                                            const DistributionStatus& distribution,
                                            bool shouldAggressivelyBalance);
};

}  // namespace mongo
