// @file balancer_policy.h

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

struct ChunkInfo {
    const BSONObj min;
    const BSONObj max;

    ChunkInfo(const BSONObj& chunk)
        : min(chunk[ChunkType::min()].Obj().getOwned()),
          max(chunk[ChunkType::max()].Obj().getOwned()) {}

    std::string toString() const;
};


struct TagRange {
    BSONObj min;
    BSONObj max;
    std::string tag;

    TagRange() {}

    TagRange(const BSONObj& a_min, const BSONObj& a_max, const std::string& a_tag)
        : min(a_min.getOwned()), max(a_max.getOwned()), tag(a_tag) {}

    std::string toString() const;
};

struct MigrateInfo {
    MigrateInfo(const std::string& a_ns,
                const ShardId& a_to,
                const ShardId& a_from,
                const BSONObj& a_chunk)
        : ns(a_ns), to(a_to), from(a_from), chunk(a_chunk) {}

    const std::string ns;
    const ShardId to;
    const ShardId from;
    const ChunkInfo chunk;
};

typedef std::vector<ClusterStatistics::ShardStatistics> ShardStatisticsVector;
typedef std::map<ShardId, std::vector<ChunkType>> ShardToChunksMap;

class DistributionStatus {
    MONGO_DISALLOW_COPYING(DistributionStatus);

public:
    DistributionStatus(ShardStatisticsVector shardInfo, const ShardToChunksMap& shardToChunksMap);

    // only used when building

    /**
     * @return if range is valid
     */
    bool addTagRange(const TagRange& range);

    // ---- these methods might be better suiting in BalancerPolicy

    /**
     * @param forTag "" if you don't care, or a tag
     * @return shard best suited to receive a chunk
     */
    std::string getBestReceieverShard(const std::string& forTag) const;

    /**
     * @return the shard with the most chunks
     *         based on # of chunks with the given tag
     */
    std::string getMostOverloadedShard(const std::string& forTag) const;


    // ---- basic accessors, counters, etc...

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

    /** writes all state to log() */
    void dump() const;

    const ShardStatisticsVector& getStats() const {
        return _shardInfo;
    }

    /**
     * Note: jumbo and versions are not set.
     */
    static void populateShardToChunksMap(const ShardStatisticsVector& allShards,
                                         const ChunkManager& chunkMgr,
                                         ShardToChunksMap* shardToChunksMap);

private:
    const ShardStatisticsVector _shardInfo;
    const ShardToChunksMap& _shardChunks;
    std::map<BSONObj, TagRange> _tagRanges;
    std::set<std::string> _allTags;
};


class BalancerPolicy {
public:
    /**
     * Returns a suggested chunk to move whithin a collection's shards, given information about
     * space usage and number of chunks for that collection. If the policy doesn't recommend
     * moving, it returns NULL.
     *
     * @param ns is the collections namepace.
     * @param DistributionStatus holds all the info about the current state of the cluster/namespace
     * @param balancedLastTime is the number of chunks effectively moved in the last round.
     * @returns NULL or MigrateInfo of the best move to make towards balacing the collection.
     *          caller owns the MigrateInfo instance
     */
    static MigrateInfo* balance(const std::string& ns,
                                const DistributionStatus& distribution,
                                int balancedLastTime);
};

}  // namespace mongo
