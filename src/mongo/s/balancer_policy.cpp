// balancer_policy.cpp
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

#include "mongo/s/balancer_policy.h"

#include <algorithm>

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/s/catalog/catalog_manager.h"
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

namespace {

/**
 * Executes the serverStatus command against the specified shard and obtains the version of the
 * running MongoD service.
 *
 * The MongoD version or throws an exception. Known exception codes are:
 *  ShardNotFound if shard by that id is not available on the registry
 *  NoSuchKey if the version could not be retrieved
 */
std::string retrieveShardMongoDVersion(OperationContext* txn,
                                       ShardId shardId,
                                       ShardRegistry* shardRegistry) {
    BSONObj serverStatus = uassertStatusOK(shardRegistry->runIdempotentCommandOnShard(
        txn,
        shardId,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        BSON("serverStatus" << 1)));
    BSONElement versionElement = serverStatus["version"];
    if (versionElement.type() != String) {
        uassertStatusOK({ErrorCodes::NoSuchKey, "version field not found in serverStatus"});
    }

    return versionElement.str();
}

}  // namespace

string TagRange::toString() const {
    return str::stream() << min << " -->> " << max << "  on  " << tag;
}

DistributionStatus::DistributionStatus(const ShardInfoMap& shardInfo,
                                       const ShardToChunksMap& shardToChunksMap)
    : _shardInfo(shardInfo), _shardChunks(shardToChunksMap) {
    for (ShardInfoMap::const_iterator i = _shardInfo.begin(); i != _shardInfo.end(); ++i) {
        _shardIds.insert(i->first);
    }
}

const ShardInfo& DistributionStatus::shardInfo(const ShardId& shardId) const {
    ShardInfoMap::const_iterator i = _shardInfo.find(shardId);
    verify(i != _shardInfo.end());
    return i->second;
}

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

string DistributionStatus::getBestReceieverShard(const string& tag) const {
    string best;
    unsigned minChunks = numeric_limits<unsigned>::max();

    for (ShardInfoMap::const_iterator i = _shardInfo.begin(); i != _shardInfo.end(); ++i) {
        if (i->second.isSizeMaxed()) {
            LOG(1) << i->first << " has already reached the maximum total chunk size.";
            continue;
        }

        if (i->second.isDraining()) {
            LOG(1) << i->first << " is currently draining.";
            continue;
        }

        if (!i->second.hasTag(tag)) {
            LOG(1) << i->first << " doesn't have right tag";
            continue;
        }

        unsigned myChunks = numberOfChunksInShard(i->first);
        if (myChunks >= minChunks) {
            LOG(1) << i->first << " has more chunks me:" << myChunks << " best: " << best << ":"
                   << minChunks;
            continue;
        }

        best = i->first;
        minChunks = myChunks;
    }

    return best;
}

string DistributionStatus::getMostOverloadedShard(const string& tag) const {
    string worst;
    unsigned maxChunks = 0;

    for (ShardInfoMap::const_iterator i = _shardInfo.begin(); i != _shardInfo.end(); ++i) {
        unsigned myChunks = numberOfChunksInShardWithTag(i->first, tag);
        if (myChunks <= maxChunks)
            continue;

        worst = i->first;
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
    if (_tagRanges.size() == 0)
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

    for (ShardInfoMap::const_iterator i = _shardInfo.begin(); i != _shardInfo.end(); ++i) {
        log() << "      " << i->first << "\t" << i->second.toString();

        ShardToChunksMap::const_iterator j = _shardChunks.find(i->first);
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

Status DistributionStatus::populateShardInfoMap(OperationContext* txn, ShardInfoMap* shardInfo) {
    try {
        auto shardsStatus = grid.catalogManager(txn)->getAllShards(txn);
        if (!shardsStatus.isOK()) {
            return shardsStatus.getStatus();
        }
        vector<ShardType> shards = std::move(shardsStatus.getValue().value);

        for (const ShardType& shardData : shards) {
            std::set<std::string> dummy;

            const long long shardSizeBytes = uassertStatusOK(
                shardutil::retrieveTotalShardSize(txn, shardData.getName(), grid.shardRegistry()));

            const std::string shardMongodVersion =
                retrieveShardMongoDVersion(txn, shardData.getName(), grid.shardRegistry());

            ShardInfo newShardEntry(shardData.getMaxSizeMB(),
                                    shardSizeBytes / 1024 / 1024,
                                    shardData.getDraining(),
                                    dummy,
                                    shardMongodVersion);

            for (const string& shardTag : shardData.getTags()) {
                newShardEntry.addTag(shardTag);
            }

            shardInfo->insert(make_pair(shardData.getName(), newShardEntry));
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

void DistributionStatus::populateShardToChunksMap(const ShardInfoMap& allShards,
                                                  const ChunkManager& chunkMgr,
                                                  ShardToChunksMap* shardToChunksMap) {
    // Makes sure there is an entry in shardToChunksMap for every shard.
    for (ShardInfoMap::const_iterator it = allShards.begin(); it != allShards.end(); ++it) {
        (*shardToChunksMap)[it->first];
    }

    const ChunkMap& chunkMap = chunkMgr.getChunkMap();
    for (ChunkMap::const_iterator it = chunkMap.begin(); it != chunkMap.end(); ++it) {
        const ChunkPtr chunkPtr = it->second;

        ChunkType chunk;
        chunk.setNS(chunkMgr.getns());
        chunk.setMin(chunkPtr->getMin().getOwned());
        chunk.setMax(chunkPtr->getMax().getOwned());
        chunk.setJumbo(chunkPtr->isJumbo());  // TODO: is this reliable?

        const string shardName(chunkPtr->getShardId());
        chunk.setShard(shardName);

        (*shardToChunksMap)[shardName].push_back(chunk);
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
        for (const ShardId& shardId : distribution.shardIds()) {
            const ShardInfo& info = distribution.shardInfo(shardId);

            if (!info.isDraining())
                continue;

            if (distribution.numberOfChunksInShard(shardId) == 0)
                continue;

            // now we know we need to move to chunks off this shard
            // we will if we are allowed
            const vector<ChunkType>& chunks = distribution.getChunks(shardId);
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

                if (to.size() == 0) {
                    warning() << "want to move chunk: " << chunkToMove << "(" << tag << ") "
                              << "from " << shardId << " but can't find anywhere to put it";
                    continue;
                }

                log() << "going to move " << chunkToMove << " from " << shardId << "(" << tag << ")"
                      << " to " << to;

                return new MigrateInfo(ns, to, shardId, chunkToMove.toBSON());
            }

            warning() << "can't find any chunk to move from: " << shardId << " but we want to. "
                      << " numJumboChunks: " << numJumboChunks;
        }
    }

    // 2) tag violations
    if (distribution.tags().size() > 0) {
        for (const ShardId& shardId : distribution.shardIds()) {
            const ShardInfo& info = distribution.shardInfo(shardId);

            const vector<ChunkType>& chunks = distribution.getChunks(shardId);
            for (unsigned j = 0; j < chunks.size(); j++) {
                const ChunkType& chunk = chunks[j];
                string tag = distribution.getTagForChunk(chunk);

                if (info.hasTag(tag))
                    continue;

                // uh oh, this chunk is in the wrong place
                log() << "chunk " << chunk << " is not on a shard with the right tag: " << tag;

                if (chunk.getJumbo()) {
                    warning() << "chunk " << chunk << " is jumbo, so cannot be moved";
                    continue;
                }

                const ShardId to = distribution.getBestReceieverShard(tag);
                if (to.size() == 0) {
                    log() << "no where to put it :(";
                    continue;
                }
                verify(to != shardId);
                log() << " going to move to: " << to;
                return new MigrateInfo(ns, to, shardId, chunk.toBSON());
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
        if (from.size() == 0)
            continue;

        unsigned max = distribution.numberOfChunksInShardWithTag(from, tag);
        if (max == 0)
            continue;

        string to = distribution.getBestReceieverShard(tag);
        if (to.size() == 0) {
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
            return new MigrateInfo(ns, to, from, chunk.toBSON());
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


ShardInfo::ShardInfo(long long maxSizeMB,
                     long long currSizeMB,
                     bool draining,
                     const set<string>& tags,
                     const string& mongoVersion)
    : _maxSizeMB(maxSizeMB),
      _currSizeMB(currSizeMB),
      _draining(draining),
      _tags(tags),
      _mongoVersion(mongoVersion) {}

ShardInfo::ShardInfo() : _maxSizeMB(0), _currSizeMB(0), _draining(false) {}

void ShardInfo::addTag(const string& tag) {
    _tags.insert(tag);
}


bool ShardInfo::isSizeMaxed() const {
    if (_maxSizeMB == 0 || _currSizeMB == 0)
        return false;

    return _currSizeMB >= _maxSizeMB;
}

bool ShardInfo::hasTag(const string& tag) const {
    if (tag.size() == 0)
        return true;
    return _tags.count(tag) > 0;
}

string ShardInfo::toString() const {
    StringBuilder ss;
    ss << " maxSizeMB: " << _maxSizeMB;
    ss << " currSizeMB: " << _currSizeMB;
    ss << " draining: " << _draining;
    if (_tags.size() > 0) {
        ss << "tags : ";
        for (set<string>::const_iterator i = _tags.begin(); i != _tags.end(); ++i)
            ss << *i << ",";
    }
    ss << " version: " << _mongoVersion;
    return ss.str();
}

string ChunkInfo::toString() const {
    StringBuilder buf;
    buf << " min: " << min;
    buf << " max: " << max;
    return buf.str();
}

}  // namespace mongo
