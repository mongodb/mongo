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

#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::map;
using std::numeric_limits;
using std::set;
using std::string;
using std::vector;

namespace {

// These values indicate the minimum deviation shard's number of chunks need to have from the
// optimal average across all shards for a zone for a rebalancing migration to be initiated.
const size_t kDefaultImbalanceThreshold = 2;
const size_t kAggressiveImbalanceThreshold = 1;

}  // namespace

DistributionStatus::DistributionStatus(NamespaceString nss, ShardToChunksMap shardToChunksMap)
    : _nss(std::move(nss)), _shardChunks(std::move(shardToChunksMap)) {}

size_t DistributionStatus::totalChunks() const {
    size_t total = 0;

    for (const auto& shardChunk : _shardChunks) {
        total += shardChunk.second.size();
    }

    return total;
}

size_t DistributionStatus::totalChunksWithTag(const std::string& tag) const {
    size_t total = 0;

    for (const auto& shardChunk : _shardChunks) {
        total += numberOfChunksInShardWithTag(shardChunk.first, tag);
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
    const auto minIntersect = _tagRanges.upper_bound(range.min);
    const auto maxIntersect = _tagRanges.upper_bound(range.max);

    // Check for partial overlap
    if (minIntersect != maxIntersect) {
        return false;
    }

    // Check for containment
    if (minIntersect != _tagRanges.end()) {
        const TagRange& nextRange = minIntersect->second;
        if (range.max > nextRange.min) {
            invariant(range.max < nextRange.max);
            return false;
        }
    }

    _tagRanges[range.max.getOwned()] = range;
    _allTags.insert(range.tag);
    return true;
}

string DistributionStatus::getTagForChunk(const ChunkType& chunk) const {
    const auto minIntersect = _tagRanges.upper_bound(chunk.getMin());
    const auto maxIntersect = _tagRanges.lower_bound(chunk.getMax());

    // We should never have a partial overlap with a chunk range. If it happens, treat it as if this
    // chunk doesn't belong to a tag
    if (minIntersect != maxIntersect) {
        return "";
    }

    if (minIntersect == _tagRanges.end()) {
        return "";
    }

    const TagRange& intersectRange = minIntersect->second;

    // Check for containment
    if (intersectRange.min <= chunk.getMin() && chunk.getMax() <= intersectRange.max) {
        return intersectRange.tag;
    }

    return "";
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
                                                     const string& tag,
                                                     const set<ShardId>& excludedShards) {
    ShardId best;
    unsigned minChunks = numeric_limits<unsigned>::max();

    for (const auto& stat : shardStats) {
        if (excludedShards.count(stat.shardId))
            continue;

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
                                                const string& chunkTag,
                                                const set<ShardId>& excludedShards) {
    ShardId worst;
    unsigned maxChunks = 0;

    for (const auto& stat : shardStats) {
        if (excludedShards.count(stat.shardId))
            continue;

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
    vector<MigrateInfo> migrations;

    // Set of shards, which have already been used for migrations. Used so we don't return multiple
    // migrations for the same shard.
    set<ShardId> usedShards;

    // 1) Check for shards, which are in draining mode and must have chunks moved off of them
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

                const ShardId to =
                    _getLeastLoadedReceiverShard(shardStats, distribution, tag, usedShards);
                if (!to.isValid()) {
                    if (migrations.empty()) {
                        warning() << "Chunk " << chunk
                                  << " is on a draining shard, but no appropriate recipient found";
                    }
                    continue;
                }

                invariant(to != stat.shardId);
                migrations.emplace_back(distribution.nss().ns(), to, chunk);
                invariant(usedShards.insert(stat.shardId).second);
                invariant(usedShards.insert(to).second);
                break;
            }

            if (migrations.empty()) {
                warning() << "Unable to find any chunk to move from draining shard " << stat.shardId
                          << ". numJumboChunks: " << numJumboChunks;
            }
        }
    }

    // 2) Check for chunks, which are on the wrong shard and must be moved off of it
    if (!distribution.tags().empty()) {
        for (const auto& stat : shardStats) {
            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);
            for (const auto& chunk : chunks) {
                const string tag = distribution.getTagForChunk(chunk);
                if (tag.empty())
                    continue;

                if (stat.shardTags.count(tag))
                    continue;

                if (chunk.getJumbo()) {
                    warning() << "chunk " << chunk << " violates tag " << tag
                              << ", but it is jumbo and cannot be moved";
                    continue;
                }

                const ShardId to =
                    _getLeastLoadedReceiverShard(shardStats, distribution, tag, usedShards);
                if (!to.isValid()) {
                    if (migrations.empty()) {
                        warning() << "chunk " << chunk << " violates tag " << tag
                                  << ", but no appropriate recipient found";
                    }
                    continue;
                }

                invariant(to != stat.shardId);
                migrations.emplace_back(distribution.nss().ns(), to, chunk);
                invariant(usedShards.insert(stat.shardId).second);
                invariant(usedShards.insert(to).second);
                break;
            }
        }
    }

    // 3) for each tag balance
    const size_t imbalanceThreshold = (shouldAggressivelyBalance || distribution.totalChunks() < 20)
        ? kAggressiveImbalanceThreshold
        : kDefaultImbalanceThreshold;

    vector<string> tagsPlusEmpty(distribution.tags().begin(), distribution.tags().end());
    tagsPlusEmpty.push_back("");

    for (const auto& tag : tagsPlusEmpty) {
        while (_singleZoneBalance(
            shardStats, distribution, tag, imbalanceThreshold, &migrations, &usedShards))
            ;
    }

    return migrations;
}

boost::optional<MigrateInfo> BalancerPolicy::balanceSingleChunk(
    const ChunkType& chunk,
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution) {
    const string tag = distribution.getTagForChunk(chunk);

    ShardId newShardId = _getLeastLoadedReceiverShard(shardStats, distribution, tag, {});
    if (!newShardId.isValid() || newShardId == chunk.getShard()) {
        return boost::optional<MigrateInfo>();
    }

    return MigrateInfo(distribution.nss().ns(), newShardId, chunk);
}

bool BalancerPolicy::_singleZoneBalance(const ShardStatisticsVector& shardStats,
                                        const DistributionStatus& distribution,
                                        const string& tag,
                                        size_t imbalanceThreshold,
                                        vector<MigrateInfo>* migrations,
                                        set<ShardId>* usedShards) {
    const ShardId from = _getMostOverloadedShard(shardStats, distribution, tag, *usedShards);
    if (!from.isValid())
        return false;

    const size_t max = distribution.numberOfChunksInShardWithTag(from, tag);
    if (max == 0)
        return false;

    const ShardId to = _getLeastLoadedReceiverShard(shardStats, distribution, tag, *usedShards);
    if (!to.isValid()) {
        if (migrations->empty()) {
            log() << "No available shards to take chunks for tag [" << tag << "]";
        }
        return false;
    }

    const size_t min = distribution.numberOfChunksInShardWithTag(to, tag);
    if (min >= max)
        return false;

    const size_t totalNumberOfChunksWithTag =
        (tag.empty() ? distribution.totalChunks() : distribution.totalChunksWithTag(tag));

    size_t totalNumberOfShardsWithTag = 0;

    for (const auto& stat : shardStats) {
        if (tag.empty() || stat.shardTags.count(tag)) {
            totalNumberOfShardsWithTag++;
        }
    }

    // totalNumberOfShardsWithTag cannot be zero if the to shard is valid
    invariant(totalNumberOfShardsWithTag);
    invariant(totalNumberOfChunksWithTag >= max);

    // The ideal should be at least one per shard
    const size_t idealNumberOfChunksPerShardWithTag =
        (totalNumberOfChunksWithTag < totalNumberOfShardsWithTag)
        ? 1
        : (totalNumberOfChunksWithTag / totalNumberOfShardsWithTag);

    const size_t imbalance = max - idealNumberOfChunksPerShardWithTag;

    LOG(1) << "collection : " << distribution.nss().ns();
    LOG(1) << "zone       : " << tag;
    LOG(1) << "donor      : " << from << " chunks on " << max;
    LOG(1) << "receiver   : " << to << " chunks on " << min;
    LOG(1) << "ideal      : " << idealNumberOfChunksPerShardWithTag;
    LOG(1) << "threshold  : " << imbalanceThreshold;

    // Check whether it is necessary to balance within this zone
    if (imbalance < imbalanceThreshold)
        return false;

    const vector<ChunkType>& chunks = distribution.getChunks(from);

    unsigned numJumboChunks = 0;

    for (const auto& chunk : chunks) {
        if (distribution.getTagForChunk(chunk) != tag)
            continue;

        if (chunk.getJumbo()) {
            numJumboChunks++;
            continue;
        }

        migrations->emplace_back(distribution.nss().ns(), to, chunk);
        invariant(usedShards->insert(chunk.getShard()).second);
        invariant(usedShards->insert(to).second);
        return true;
    }

    if (numJumboChunks) {
        warning() << "Shard: " << from << ", collection: " << distribution.nss().ns()
                  << " has only jumbo chunks for zone \'" << tag
                  << "\' and cannot be balanced. Jumbo chunks count: " << numJumboChunks;
    }

    return false;
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
