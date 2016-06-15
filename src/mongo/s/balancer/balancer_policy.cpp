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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/balancer/balancer_policy.h"

#include <algorithm>

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::map;
using std::numeric_limits;
using std::set;
using std::string;
using std::vector;

DistributionStatus::DistributionStatus(ShardStatisticsVector shardInfo,
                                       const ShardToChunksMap& shardToChunksMap)
    : _shardInfo(std::move(shardInfo)), _shardChunks(shardToChunksMap) {}

unsigned DistributionStatus::totalChunks() const {
    unsigned total = 0;

    for (ShardToChunksMap::const_iterator i = _shardChunks.begin(); i != _shardChunks.end(); ++i) {
        total += i->second.size();
    }

    return total;
}

unsigned DistributionStatus::numberOfChunksInShard(const ShardId& shardId) const {
    ShardToChunksMap::const_iterator i = _shardChunks.find(shardId);
    if (i == _shardChunks.end()) {
        return 0;
    }

    return i->second.size();
}

unsigned DistributionStatus::numberOfChunksInShardWithTag(const ShardId& shardId,
                                                          const string& tag) const {
    ShardToChunksMap::const_iterator i = _shardChunks.find(shardId);
    if (i == _shardChunks.end()) {
        return 0;
    }

    unsigned total = 0;

    const vector<ChunkType>& chunkList = i->second;
    for (unsigned j = 0; j < i->second.size(); j++) {
        if (tag == getTagForChunk(chunkList[j])) {
            total++;
        }
    }

    return total;
}

Status DistributionStatus::isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                                   const string& chunkTag) {
    if (stat.isSizeMaxed()) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId
                              << " has already reached the maximum total chunk size."};
    }

    if (stat.isDraining) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is currently draining."};
    }

    if (!chunkTag.empty() && !stat.shardTags.count(chunkTag)) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " doesn't have right tag"};
    }

    return Status::OK();
}

ShardId DistributionStatus::getBestReceieverShard(const string& tag) const {
    ShardId best;
    unsigned minChunks = numeric_limits<unsigned>::max();

    for (const auto& stat : _shardInfo) {
        auto status = isShardSuitableReceiver(stat, tag);
        if (!status.isOK()) {
            LOG(1) << status.codeString();
            continue;
        }

        unsigned myChunks = numberOfChunksInShard(stat.shardId);
        if (myChunks >= minChunks) {
            LOG(1) << stat.shardId << " has more chunks me:" << myChunks << " best: " << best << ":"
                   << minChunks;
            continue;
        }

        best = stat.shardId;
        minChunks = myChunks;
    }

    return best;
}

ShardId DistributionStatus::getMostOverloadedShard(const string& tag) const {
    ShardId worst;
    unsigned maxChunks = 0;

    for (const auto& stat : _shardInfo) {
        unsigned myChunks = numberOfChunksInShardWithTag(stat.shardId, tag);
        if (myChunks <= maxChunks)
            continue;

        worst = stat.shardId;
        maxChunks = myChunks;
    }

    return worst;
}

const vector<ChunkType>& DistributionStatus::getChunks(const ShardId& shardId) const {
    ShardToChunksMap::const_iterator i = _shardChunks.find(shardId);
    invariant(i != _shardChunks.end());

    return i->second;
}

bool DistributionStatus::addTagRange(const TagRange& range) {
    // first check for overlaps
    for (map<BSONObj, TagRange>::const_iterator i = _tagRanges.begin(); i != _tagRanges.end();
         ++i) {
        const TagRange& tocheck = i->second;

        if (range.min == tocheck.min) {
            LOG(1) << "have 2 ranges with the same min " << range << " " << tocheck;
            return false;
        }

        if (range.min < tocheck.min) {
            if (range.max > tocheck.min) {
                LOG(1) << "have overlapping ranges " << range << " " << tocheck;
                return false;
            }
        } else {
            // range.min > tocheck.min
            if (tocheck.max > range.min) {
                LOG(1) << "have overlapping ranges " << range << " " << tocheck;
                return false;
            }
        }
    }

    _tagRanges[range.max.getOwned()] = range;
    _allTags.insert(range.tag);

    return true;
}

string DistributionStatus::getTagForChunk(const ChunkType& chunk) const {
    if (_tagRanges.empty())
        return "";

    const BSONObj min(chunk.getMin());

    map<BSONObj, TagRange>::const_iterator i = _tagRanges.upper_bound(min);
    if (i == _tagRanges.end())
        return "";

    const TagRange& range = i->second;
    if (min < range.min)
        return "";

    return range.tag;
}

void DistributionStatus::dump() const {
    log() << "DistributionStatus";
    log() << "  shards";

    for (const auto& stat : _shardInfo) {
        log() << "      " << stat.shardId << "\t" << stat.toBSON();

        ShardToChunksMap::const_iterator j = _shardChunks.find(stat.shardId);
        verify(j != _shardChunks.end());

        const vector<ChunkType>& v = j->second;
        for (unsigned x = 0; x < v.size(); x++) {
            log() << "          " << v[x];
        }
    }

    if (_tagRanges.size() > 0) {
        log() << " tag ranges";

        for (map<BSONObj, TagRange>::const_iterator i = _tagRanges.begin(); i != _tagRanges.end();
             ++i)
            log() << i->second.toString();
    }
}

MigrateInfo* BalancerPolicy::balance(const string& ns,
                                     const DistributionStatus& distribution,
                                     int balancedLastTime) {
    // 1) check for shards that policy require to us to move off of:
    //    draining only
    // 2) check tag policy violations
    // 3) then we make sure chunks are balanced for each tag

    // ----

    // 1) check things we have to move
    {
        for (const auto& stat : distribution.getStats()) {
            if (!stat.isDraining)
                continue;

            if (distribution.numberOfChunksInShard(stat.shardId) == 0)
                continue;

            // now we know we need to move to chunks off this shard
            // we will if we are allowed
            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);
            unsigned numJumboChunks = 0;

            // since we have to move all chunks, lets just do in order
            for (unsigned i = 0; i < chunks.size(); i++) {
                const ChunkType& chunkToMove = chunks[i];
                if (chunkToMove.getJumbo()) {
                    numJumboChunks++;
                    continue;
                }

                string tag = distribution.getTagForChunk(chunkToMove);
                const ShardId to = distribution.getBestReceieverShard(tag);

                if (!to.isValid()) {
                    warning() << "want to move chunk: " << chunkToMove << "(" << tag << ") "
                              << "from " << stat.shardId << " but can't find anywhere to put it";
                    continue;
                }

                log() << "going to move " << chunkToMove << " from " << stat.shardId << "(" << tag
                      << ")"
                      << " to " << to;

                return new MigrateInfo(ns, to, chunkToMove);
            }

            warning() << "can't find any chunk to move from: " << stat.shardId
                      << " but we want to. "
                      << " numJumboChunks: " << numJumboChunks;
        }
    }

    // 2) tag violations
    if (distribution.tags().size() > 0) {
        for (const auto& stat : distribution.getStats()) {
            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);
            for (unsigned j = 0; j < chunks.size(); j++) {
                const ChunkType& chunk = chunks[j];
                string tag = distribution.getTagForChunk(chunk);

                if (tag.empty() || stat.shardTags.count(tag))
                    continue;

                // uh oh, this chunk is in the wrong place
                log() << "chunk " << chunk << " is not on a shard with the right tag: " << tag;

                if (chunk.getJumbo()) {
                    warning() << "chunk " << chunk << " is jumbo, so cannot be moved";
                    continue;
                }

                const ShardId to = distribution.getBestReceieverShard(tag);
                if (!to.isValid()) {
                    log() << "no where to put it :(";
                    continue;
                }

                invariant(to != stat.shardId);
                log() << " going to move to: " << to;
                return new MigrateInfo(ns, to, chunk);
            }
        }
    }

    // 3) for each tag balance

    int threshold = 8;
    if (balancedLastTime || distribution.totalChunks() < 20)
        threshold = 2;
    else if (distribution.totalChunks() < 80)
        threshold = 4;

    // randomize the order in which we balance the tags
    // this is so that one bad tag doesn't prevent others from getting balanced
    vector<string> tags;
    {
        set<string> t = distribution.tags();
        for (set<string>::const_iterator i = t.begin(); i != t.end(); ++i)
            tags.push_back(*i);
        tags.push_back("");

        std::random_shuffle(tags.begin(), tags.end());
    }

    for (unsigned i = 0; i < tags.size(); i++) {
        string tag = tags[i];

        const ShardId from = distribution.getMostOverloadedShard(tag);
        if (!from.isValid())
            continue;

        unsigned max = distribution.numberOfChunksInShardWithTag(from, tag);
        if (max == 0)
            continue;

        ShardId to = distribution.getBestReceieverShard(tag);
        if (!to.isValid()) {
            log() << "no available shards to take chunks for tag [" << tag << "]";
            return NULL;
        }

        unsigned min = distribution.numberOfChunksInShardWithTag(to, tag);

        const int imbalance = max - min;

        LOG(1) << "collection : " << ns;
        LOG(1) << "donor      : " << from << " chunks on " << max;
        LOG(1) << "receiver   : " << to << " chunks on " << min;
        LOG(1) << "threshold  : " << threshold;

        if (imbalance < threshold)
            continue;

        const vector<ChunkType>& chunks = distribution.getChunks(from);
        unsigned numJumboChunks = 0;
        for (unsigned j = 0; j < chunks.size(); j++) {
            const ChunkType& chunk = chunks[j];
            if (distribution.getTagForChunk(chunk) != tag)
                continue;

            if (chunk.getJumbo()) {
                numJumboChunks++;
                continue;
            }

            log() << " ns: " << ns << " going to move " << chunk << " from: " << from
                  << " to: " << to << " tag [" << tag << "]";
            return new MigrateInfo(ns, to, chunk);
        }

        if (numJumboChunks) {
            error() << "shard: " << from << " ns: " << ns
                    << " has too many chunks, but they are all jumbo "
                    << " numJumboChunks: " << numJumboChunks;
            continue;
        }

        verify(false);  // should be impossible
    }

    // Everything is balanced here!
    return NULL;
}

string TagRange::toString() const {
    return str::stream() << min << " -->> " << max << "  on  " << tag;
}

string MigrateInfo::toString() const {
    return str::stream() << ns << ": [" << minKey << ", " << maxKey << "), from " << from << ", to "
                         << to;
}

}  // namespace mongo
