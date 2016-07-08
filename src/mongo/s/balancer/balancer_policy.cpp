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

DistributionStatus::DistributionStatus(NamespaceString nss, ShardToChunksMap shardToChunksMap)
    : _nss(std::move(nss)), _shardChunks(std::move(shardToChunksMap)) {}

size_t DistributionStatus::totalChunks() const {
    size_t total = 0;

    for (const auto& shardChunk : _shardChunks) {
        total += shardChunk.second.size();
    }

    return total;
}

size_t DistributionStatus::numberOfChunksInShard(const ShardId& shardId) const {
    const auto& shardChunks = getChunks(shardId);
    return shardChunks.size();
}

size_t DistributionStatus::numberOfChunksInShardWithTag(const ShardId& shardId,
                                                        const string& tag) const {
    const auto& shardChunks = getChunks(shardId);

    size_t total = 0;

    for (const auto& chunk : shardChunks) {
        if (tag == getTagForChunk(chunk)) {
            total++;
        }
    }

    return total;
}

const vector<ChunkType>& DistributionStatus::getChunks(const ShardId& shardId) const {
    ShardToChunksMap::const_iterator i = _shardChunks.find(shardId);
    invariant(i != _shardChunks.end());

    return i->second;
}

bool DistributionStatus::addTagRange(const TagRange& range) {
    // Check for overlaps
    for (const auto& tagRangesEntry : _tagRanges) {
        const TagRange& tocheck = tagRangesEntry.second;

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

void DistributionStatus::report(BSONObjBuilder* builder) const {
    builder->append("ns", _nss.ns());

    // Report all shards
    BSONArrayBuilder shardArr(builder->subarrayStart("shards"));
    for (const auto& shardChunk : _shardChunks) {
        BSONObjBuilder shardEntry(shardArr.subobjStart());
        shardEntry.append("name", shardChunk.first.toString());

        BSONArrayBuilder chunkArr(shardEntry.subarrayStart("chunks"));
        for (const auto& chunk : shardChunk.second) {
            chunkArr.append(chunk.toBSON());
        }
        chunkArr.doneFast();

        shardEntry.doneFast();
    }
    shardArr.doneFast();

    // Report all tags
    BSONArrayBuilder tagsArr(builder->subarrayStart("tags"));
    tagsArr.append(_allTags);
    tagsArr.doneFast();

    // Report all tag ranges
    BSONArrayBuilder tagRangesArr(builder->subarrayStart("tagRanges"));
    for (const auto& tagRange : _tagRanges) {
        BSONObjBuilder tagRangeEntry(tagRangesArr.subobjStart());
        tagRangeEntry.append("tag", tagRange.second.tag);
        tagRangeEntry.append("mapKey", tagRange.first);
        tagRangeEntry.append("min", tagRange.second.min);
        tagRangeEntry.append("max", tagRange.second.max);
        tagRangeEntry.doneFast();
    }
    tagRangesArr.doneFast();
}

string DistributionStatus::toString() const {
    BSONObjBuilder builder;
    report(&builder);

    return builder.obj().toString();
}

Status BalancerPolicy::isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
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

ShardId BalancerPolicy::_getLeastLoadedReceiverShard(const ShardStatisticsVector& shardStats,
                                                     const DistributionStatus& distribution,
                                                     const string& tag) {
    ShardId best;
    unsigned minChunks = numeric_limits<unsigned>::max();

    for (const auto& stat : shardStats) {
        auto status = isShardSuitableReceiver(stat, tag);
        if (!status.isOK()) {
            continue;
        }

        unsigned myChunks = distribution.numberOfChunksInShard(stat.shardId);
        if (myChunks >= minChunks) {
            continue;
        }

        best = stat.shardId;
        minChunks = myChunks;
    }

    return best;
}

ShardId BalancerPolicy::_getMostOverloadedShard(const ShardStatisticsVector& shardStats,
                                                const DistributionStatus& distribution,
                                                const string& chunkTag) {
    ShardId worst;
    unsigned maxChunks = 0;

    for (const auto& stat : shardStats) {
        const unsigned shardChunkCount =
            distribution.numberOfChunksInShardWithTag(stat.shardId, chunkTag);
        if (shardChunkCount <= maxChunks)
            continue;

        worst = stat.shardId;
        maxChunks = shardChunkCount;
    }

    return worst;
}

vector<MigrateInfo> BalancerPolicy::balance(const ShardStatisticsVector& shardStats,
                                            const DistributionStatus& distribution,
                                            bool shouldAggressivelyBalance) {
    // 1) check for shards that policy require to us to move off of:
    //    draining only
    // 2) check tag policy violations
    // 3) then we make sure chunks are balanced for each tag

    // ----

    // 1) check things we have to move
    {
        for (const auto& stat : shardStats) {
            if (!stat.isDraining)
                continue;

            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);

            if (chunks.empty())
                continue;

            // Now we know we need to move to chunks off this shard, but only if permitted by the
            // tags policy
            unsigned numJumboChunks = 0;

            // Since we have to move all chunks, lets just do in order
            for (const auto& chunk : chunks) {
                if (chunk.getJumbo()) {
                    numJumboChunks++;
                    continue;
                }

                const string tag = distribution.getTagForChunk(chunk);

                const ShardId to = _getLeastLoadedReceiverShard(shardStats, distribution, tag);
                if (!to.isValid()) {
                    warning() << "chunk " << chunk
                              << " is on a draining shard, but no appropriate recipient found";
                    continue;
                }

                log() << "going to move " << chunk << " from " << stat.shardId << " (" << tag
                      << ") to " << to;

                return {MigrateInfo(distribution.nss().ns(), to, chunk)};
            }

            warning() << "can't find any chunk to move from: " << stat.shardId
                      << " but we want to. "
                      << " numJumboChunks: " << numJumboChunks;
        }
    }

    // 2) tag violations
    if (!distribution.tags().empty()) {
        for (const auto& stat : shardStats) {
            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);
            for (const auto& chunk : chunks) {
                const string tag = distribution.getTagForChunk(chunk);
                if (tag.empty() || stat.shardTags.count(tag))
                    continue;

                if (chunk.getJumbo()) {
                    warning() << "chunk " << chunk << " violates tag " << tag
                              << ", but it is jumbo and cannot be moved";
                    continue;
                }

                const ShardId to = _getLeastLoadedReceiverShard(shardStats, distribution, tag);
                if (!to.isValid()) {
                    warning() << "chunk " << chunk << " violates tag " << tag
                              << ", but no appropriate recipient found";
                    continue;
                }

                invariant(to != stat.shardId);

                log() << "going to move " << chunk << " from " << stat.shardId << " (" << tag
                      << ") to " << to;

                return {MigrateInfo(distribution.nss().ns(), to, chunk)};
            }
        }
    }

    // 3) for each tag balance
    int threshold = 8;

    if (shouldAggressivelyBalance || distribution.totalChunks() < 20) {
        threshold = 2;
    } else if (distribution.totalChunks() < 80) {
        threshold = 4;
    }

    // Randomize the order in which we balance the tags so that one bad tag doesn't prevent others
    // from getting balanced
    vector<string> tagsPlusEmpty;
    {
        for (const auto& tag : distribution.tags()) {
            tagsPlusEmpty.push_back(tag);
        }
        tagsPlusEmpty.push_back("");

        std::random_shuffle(tagsPlusEmpty.begin(), tagsPlusEmpty.end());
    }

    for (const auto& tag : tagsPlusEmpty) {
        const ShardId from = _getMostOverloadedShard(shardStats, distribution, tag);
        if (!from.isValid())
            continue;

        const unsigned max = distribution.numberOfChunksInShardWithTag(from, tag);
        if (max == 0)
            continue;

        const ShardId to = _getLeastLoadedReceiverShard(shardStats, distribution, tag);
        if (!to.isValid()) {
            log() << "no available shards to take chunks for tag [" << tag << "]";
            return vector<MigrateInfo>();
        }

        const unsigned min = distribution.numberOfChunksInShardWithTag(to, tag);

        const int imbalance = max - min;

        LOG(1) << "collection : " << distribution.nss().ns();
        LOG(1) << "donor      : " << from << " chunks on " << max;
        LOG(1) << "receiver   : " << to << " chunks on " << min;
        LOG(1) << "threshold  : " << threshold;

        if (imbalance < threshold)
            continue;

        const vector<ChunkType>& chunks = distribution.getChunks(from);
        unsigned numJumboChunks = 0;

        for (const auto& chunk : chunks) {
            if (distribution.getTagForChunk(chunk) != tag)
                continue;

            if (chunk.getJumbo()) {
                numJumboChunks++;
                continue;
            }

            log() << " ns: " << distribution.nss().ns() << " going to move " << chunk
                  << " from: " << from << " to: " << to << " tag [" << tag << "]";

            return {MigrateInfo(distribution.nss().ns(), to, chunk)};
        }

        if (numJumboChunks) {
            error() << "shard: " << from << " ns: " << distribution.nss().ns()
                    << " has too many chunks, but they are all jumbo "
                    << " numJumboChunks: " << numJumboChunks;
            continue;
        }

        MONGO_UNREACHABLE;
    }

    // Everything is balanced here!
    return vector<MigrateInfo>();
}

boost::optional<MigrateInfo> BalancerPolicy::balanceSingleChunk(
    const ChunkType& chunk,
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution) {
    const string tag = distribution.getTagForChunk(chunk);

    ShardId newShardId = _getLeastLoadedReceiverShard(shardStats, distribution, tag);
    if (!newShardId.isValid() || newShardId == chunk.getShard()) {
        return boost::optional<MigrateInfo>();
    }

    return MigrateInfo(distribution.nss().ns(), newShardId, chunk);
}

string TagRange::toString() const {
    return str::stream() << min << " -->> " << max << "  on  " << tag;
}

std::string MigrateInfo::getName() const {
    return ChunkType::genID(ns, minKey);
}

string MigrateInfo::toString() const {
    return str::stream() << ns << ": [" << minKey << ", " << maxKey << "), from " << from << ", to "
                         << to;
}

}  // namespace mongo
