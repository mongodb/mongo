/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/s/collection_metadata.h"

#include "mongo/bson/util/builder.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::make_pair;
using std::string;
using std::vector;
using str::stream;

CollectionMetadata::CollectionMetadata() = default;

CollectionMetadata::~CollectionMetadata() = default;

std::unique_ptr<CollectionMetadata> CollectionMetadata::cloneMigrate(
    const ChunkType& chunk, const ChunkVersion& newCollectionVersion) const {
    invariant(newCollectionVersion.epoch() == _collVersion.epoch());
    invariant(newCollectionVersion > _collVersion);
    invariant(rangeMapContains(_chunksMap, chunk.getMin(), chunk.getMax()));

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_chunksMap = _chunksMap;
    metadata->_chunksMap.erase(chunk.getMin());

    metadata->_shardVersion =
        (metadata->_chunksMap.empty() ? ChunkVersion(0, 0, newCollectionVersion.epoch())
                                      : newCollectionVersion);
    metadata->_collVersion = newCollectionVersion;
    metadata->fillRanges();

    invariant(metadata->isValid());
    return metadata;
}

unique_ptr<CollectionMetadata> CollectionMetadata::clonePlusChunk(
    const BSONObj& minKey, const BSONObj& maxKey, const ChunkVersion& newShardVersion) const {
    invariant(newShardVersion.epoch() == _shardVersion.epoch());
    invariant(newShardVersion.isSet());
    invariant(minKey.woCompare(maxKey) < 0);
    invariant(!rangeMapOverlaps(_chunksMap, minKey, maxKey));

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_chunksMap = _chunksMap;
    metadata->_chunksMap.insert(make_pair(minKey.getOwned(), maxKey.getOwned()));
    metadata->_shardVersion = newShardVersion;
    metadata->_collVersion = newShardVersion > _collVersion ? newShardVersion : _collVersion;
    metadata->fillRanges();

    invariant(metadata->isValid());
    return metadata;
}

std::unique_ptr<CollectionMetadata> CollectionMetadata::cloneMinusPending(
    const ChunkType& chunk) const {
    invariant(rangeMapContains(_pendingMap, chunk.getMin(), chunk.getMax()));

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_pendingMap.erase(chunk.getMin());

    metadata->_chunksMap = _chunksMap;
    metadata->_rangesMap = _rangesMap;
    metadata->_shardVersion = _shardVersion;
    metadata->_collVersion = _collVersion;

    invariant(metadata->isValid());
    return metadata;
}

std::unique_ptr<CollectionMetadata> CollectionMetadata::clonePlusPending(
    const ChunkType& chunk) const {
    invariant(!rangeMapOverlaps(_chunksMap, chunk.getMin(), chunk.getMax()));

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_chunksMap = _chunksMap;
    metadata->_rangesMap = _rangesMap;
    metadata->_shardVersion = _shardVersion;
    metadata->_collVersion = _collVersion;

    // If there are any pending chunks on the interval to be added this is ok, since pending
    // chunks aren't officially tracked yet and something may have changed on servers we do not
    // see yet.
    // We remove any chunks we overlap, the remote request starting a chunk migration must have
    // been authoritative.

    if (rangeMapOverlaps(_pendingMap, chunk.getMin(), chunk.getMax())) {
        RangeVector pendingOverlap;
        getRangeMapOverlap(_pendingMap, chunk.getMin(), chunk.getMax(), &pendingOverlap);

        warning() << "new pending chunk " << rangeToString(chunk.getMin(), chunk.getMax())
                  << " overlaps existing pending chunks " << overlapToString(pendingOverlap)
                  << ", a migration may not have completed";

        for (RangeVector::iterator it = pendingOverlap.begin(); it != pendingOverlap.end(); ++it) {
            metadata->_pendingMap.erase(it->first);
        }
    }

    metadata->_pendingMap.insert(make_pair(chunk.getMin(), chunk.getMax()));

    invariant(metadata->isValid());
    return metadata;
}

StatusWith<std::unique_ptr<CollectionMetadata>> CollectionMetadata::cloneSplit(
    const BSONObj& minKey,
    const BSONObj& maxKey,
    const std::vector<BSONObj>& splitKeys,
    const ChunkVersion& newShardVersion) const {
    invariant(newShardVersion.epoch() == _shardVersion.epoch());
    invariant(newShardVersion > _shardVersion);

    // The version required in both resulting chunks could be simply an increment in the
    // minor portion of the current version.  However, we are enforcing uniqueness over the
    // attributes <ns, version> of the configdb collection 'chunks'.  So in practice, a
    // migrate somewhere may force this split to pick up a version that has the major
    // portion higher than the one that this shard has been using.
    //
    // TODO drop the uniqueness constraint and tighten the check below so that only the
    // minor portion of version changes

    // Check that we have the exact chunk that will be subtracted.
    if (!rangeMapContains(_chunksMap, minKey, maxKey)) {
        stream errMsg;
        errMsg << "cannot split chunk " << rangeToString(minKey, maxKey)
               << ", this shard does not contain the chunk";

        if (rangeMapOverlaps(_chunksMap, minKey, maxKey)) {
            RangeVector overlap;
            getRangeMapOverlap(_chunksMap, minKey, maxKey, &overlap);

            errMsg << " and it overlaps " << overlapToString(overlap);
        }

        return {ErrorCodes::IllegalOperation, errMsg};
    }

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_chunksMap = _chunksMap;
    metadata->_shardVersion = newShardVersion;  // will increment 2nd, 3rd,... chunks below

    BSONObj startKey = minKey;
    for (const auto& split : splitKeys) {
        // Check that the split key is valid
        if (!rangeContains(minKey, maxKey, split)) {
            return {ErrorCodes::IllegalOperation,
                    stream() << "cannot split chunk " << rangeToString(minKey, maxKey) << " at key "
                             << split};
        }

        // Check that the split keys are in order
        if (split.woCompare(startKey) <= 0) {
            // The split keys came in out of order, this probably indicates a bug, so fail the
            // operation. Re-iterate splitKeys to build a useful error message including the array
            // of splitKeys in the order received.
            str::stream errMsg;
            errMsg << "Invalid input to splitChunk, split keys must be in order, got: [";
            for (auto it2 = splitKeys.cbegin(); it2 != splitKeys.cend(); ++it2) {
                if (it2 != splitKeys.begin()) {
                    errMsg << ", ";
                }
                errMsg << it2->toString();
            }
            errMsg << "]";
            return {ErrorCodes::IllegalOperation, errMsg};
        }

        metadata->_chunksMap[startKey] = split.getOwned();
        metadata->_chunksMap.insert(make_pair(split.getOwned(), maxKey.getOwned()));
        metadata->_shardVersion.incMinor();
        startKey = split;
    }

    metadata->_collVersion =
        metadata->_shardVersion > _collVersion ? metadata->_shardVersion : _collVersion;
    metadata->fillRanges();

    invariant(metadata->isValid());
    return std::move(metadata);
}

StatusWith<std::unique_ptr<CollectionMetadata>> CollectionMetadata::cloneMerge(
    const BSONObj& minKey, const BSONObj& maxKey, const ChunkVersion& newShardVersion) const {
    invariant(newShardVersion.epoch() == _shardVersion.epoch());
    invariant(newShardVersion > _shardVersion);

    RangeVector overlap;
    getRangeMapOverlap(_chunksMap, minKey, maxKey, &overlap);

    if (overlap.empty() || overlap.size() == 1) {
        return {ErrorCodes::IllegalOperation,
                stream() << "cannot merge range " << rangeToString(minKey, maxKey)
                         << (overlap.empty() ? ", no chunks found in this range"
                                             : ", only one chunk found in this range")};
    }

    bool validStartEnd = true;
    bool validNoHoles = true;

    if (overlap.begin()->first.woCompare(minKey) != 0) {
        // First chunk doesn't start with minKey
        validStartEnd = false;
    } else if (overlap.rbegin()->second.woCompare(maxKey) != 0) {
        // Last chunk doesn't end with maxKey
        validStartEnd = false;
    } else {
        // Check that there are no holes
        BSONObj prevMaxKey = minKey;
        for (RangeVector::iterator it = overlap.begin(); it != overlap.end(); ++it) {
            if (it->first.woCompare(prevMaxKey) != 0) {
                validNoHoles = false;
                break;
            }
            prevMaxKey = it->second;
        }
    }

    if (!validStartEnd || !validNoHoles) {
        return {ErrorCodes::IllegalOperation,
                stream() << "cannot merge range " << rangeToString(minKey, maxKey)
                         << ", overlapping chunks "
                         << overlapToString(overlap)
                         << (!validStartEnd ? " do not have the same min and max key"
                                            : " are not all adjacent")};
    }

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_chunksMap = _chunksMap;
    metadata->_rangesMap = _rangesMap;
    metadata->_shardVersion = newShardVersion;
    metadata->_collVersion = newShardVersion > _collVersion ? newShardVersion : this->_collVersion;

    for (RangeVector::iterator it = overlap.begin(); it != overlap.end(); ++it) {
        metadata->_chunksMap.erase(it->first);
    }

    metadata->_chunksMap.insert(make_pair(minKey, maxKey));

    invariant(metadata->isValid());
    return std::move(metadata);
}

bool CollectionMetadata::keyBelongsToMe(const BSONObj& key) const {
    // For now, collections don't move. So if the collection is not sharded, assume
    // the document with the given key can be accessed.
    if (_keyPattern.isEmpty()) {
        return true;
    }

    if (_rangesMap.size() <= 0) {
        return false;
    }

    RangeMap::const_iterator it = _rangesMap.upper_bound(key);
    if (it != _rangesMap.begin())
        it--;

    bool good = rangeContains(it->first, it->second, key);

#if 0
        // DISABLED because of SERVER-11175 - huge amount of logging
        // Logs if the point doesn't belong here.
        if ( !good ) {
            log() << "bad: " << key << " " << it->first << " " << key.woCompare( it->first ) << " "
                  << key.woCompare( it->second );

            for ( RangeMap::const_iterator i = _rangesMap.begin(); i != _rangesMap.end(); ++i ) {
                log() << "\t" << i->first << "\t" << i->second << "\t";
            }
        }
#endif

    return good;
}

bool CollectionMetadata::keyIsPending(const BSONObj& key) const {
    // If we aren't sharded, then the key is never pending (though it belongs-to-me)
    if (_keyPattern.isEmpty()) {
        return false;
    }

    if (_pendingMap.size() <= 0) {
        return false;
    }

    RangeMap::const_iterator it = _pendingMap.upper_bound(key);
    if (it != _pendingMap.begin())
        it--;

    bool isPending = rangeContains(it->first, it->second, key);
    return isPending;
}

bool CollectionMetadata::getNextChunk(const BSONObj& lookupKey, ChunkType* chunk) const {
    RangeMap::const_iterator upperChunkIt = _chunksMap.upper_bound(lookupKey);
    RangeMap::const_iterator lowerChunkIt = upperChunkIt;

    if (upperChunkIt != _chunksMap.begin()) {
        --lowerChunkIt;
    } else {
        lowerChunkIt = _chunksMap.end();
    }

    if (lowerChunkIt != _chunksMap.end() && lowerChunkIt->second.woCompare(lookupKey) > 0) {
        chunk->setMin(lowerChunkIt->first);
        chunk->setMax(lowerChunkIt->second);
        return true;
    }

    if (upperChunkIt != _chunksMap.end()) {
        chunk->setMin(upperChunkIt->first);
        chunk->setMax(upperChunkIt->second);
        return true;
    }

    return false;
}

bool CollectionMetadata::getDifferentChunk(const BSONObj& chunkMinKey,
                                           ChunkType* differentChunk) const {
    RangeMap::const_iterator upperChunkIt = _chunksMap.end();
    RangeMap::const_iterator lowerChunkIt = _chunksMap.begin();

    while (lowerChunkIt != upperChunkIt) {
        if (lowerChunkIt->first.woCompare(chunkMinKey) != 0) {
            differentChunk->setMin(lowerChunkIt->first);
            differentChunk->setMax(lowerChunkIt->second);
            return true;
        }
        ++lowerChunkIt;
    }

    return false;
}

BSONObj CollectionMetadata::toBSON() const {
    BSONObjBuilder bb;
    toBSON(bb);
    return bb.obj();
}

void CollectionMetadata::toBSONChunks(BSONArrayBuilder& bb) const {
    if (_chunksMap.empty())
        return;

    for (RangeMap::const_iterator it = _chunksMap.begin(); it != _chunksMap.end(); ++it) {
        BSONArrayBuilder chunkBB(bb.subarrayStart());
        chunkBB.append(it->first);
        chunkBB.append(it->second);
        chunkBB.done();
    }
}

void CollectionMetadata::toBSONPending(BSONArrayBuilder& bb) const {
    if (_pendingMap.empty())
        return;

    for (RangeMap::const_iterator it = _pendingMap.begin(); it != _pendingMap.end(); ++it) {
        BSONArrayBuilder pendingBB(bb.subarrayStart());
        pendingBB.append(it->first);
        pendingBB.append(it->second);
        pendingBB.done();
    }
}

void CollectionMetadata::toBSON(BSONObjBuilder& bb) const {
    _collVersion.addToBSON(bb, "collVersion");
    _shardVersion.addToBSON(bb, "shardVersion");
    bb.append("keyPattern", _keyPattern);

    BSONArrayBuilder chunksBB(bb.subarrayStart("chunks"));
    toBSONChunks(chunksBB);
    chunksBB.done();

    BSONArrayBuilder pendingBB(bb.subarrayStart("pending"));
    toBSONPending(pendingBB);
    pendingBB.done();
}

bool CollectionMetadata::getNextOrphanRange(const BSONObj& origLookupKey, KeyRange* range) const {
    if (_keyPattern.isEmpty())
        return false;

    BSONObj lookupKey = origLookupKey;
    BSONObj maxKey = getMaxKey();  // so we don't keep rebuilding
    while (lookupKey.woCompare(maxKey) < 0) {
        RangeMap::const_iterator lowerChunkIt = _chunksMap.end();
        RangeMap::const_iterator upperChunkIt = _chunksMap.end();

        if (!_chunksMap.empty()) {
            upperChunkIt = _chunksMap.upper_bound(lookupKey);
            lowerChunkIt = upperChunkIt;
            if (upperChunkIt != _chunksMap.begin())
                --lowerChunkIt;
            else
                lowerChunkIt = _chunksMap.end();
        }

        // If we overlap, continue after the overlap
        // TODO: Could optimize slightly by finding next non-contiguous chunk
        if (lowerChunkIt != _chunksMap.end() && lowerChunkIt->second.woCompare(lookupKey) > 0) {
            lookupKey = lowerChunkIt->second;
            continue;
        }

        RangeMap::const_iterator lowerPendingIt = _pendingMap.end();
        RangeMap::const_iterator upperPendingIt = _pendingMap.end();

        if (!_pendingMap.empty()) {
            upperPendingIt = _pendingMap.upper_bound(lookupKey);
            lowerPendingIt = upperPendingIt;
            if (upperPendingIt != _pendingMap.begin())
                --lowerPendingIt;
            else
                lowerPendingIt = _pendingMap.end();
        }

        // If we overlap, continue after the overlap
        // TODO: Could optimize slightly by finding next non-contiguous chunk
        if (lowerPendingIt != _pendingMap.end() &&
            lowerPendingIt->second.woCompare(lookupKey) > 0) {
            lookupKey = lowerPendingIt->second;
            continue;
        }

        //
        // We know that the lookup key is not covered by a chunk or pending range, and where the
        // previous chunk and pending chunks are.  Now we fill in the bounds as the closest
        // bounds of the surrounding ranges in both maps.
        //

        range->keyPattern = _keyPattern;
        range->minKey = getMinKey();
        range->maxKey = maxKey;

        if (lowerChunkIt != _chunksMap.end() && lowerChunkIt->second.woCompare(range->minKey) > 0) {
            range->minKey = lowerChunkIt->second;
        }

        if (upperChunkIt != _chunksMap.end() && upperChunkIt->first.woCompare(range->maxKey) < 0) {
            range->maxKey = upperChunkIt->first;
        }

        if (lowerPendingIt != _pendingMap.end() &&
            lowerPendingIt->second.woCompare(range->minKey) > 0) {
            range->minKey = lowerPendingIt->second;
        }

        if (upperPendingIt != _pendingMap.end() &&
            upperPendingIt->first.woCompare(range->maxKey) < 0) {
            range->maxKey = upperPendingIt->first;
        }

        return true;
    }

    return false;
}

string CollectionMetadata::toString() const {
    StringBuilder ss;
    ss << " CollectionManager version: " << _shardVersion.toString() << " key: " << _keyPattern;
    if (_rangesMap.empty()) {
        return ss.str();
    }

    RangeMap::const_iterator it = _rangesMap.begin();
    ss << it->first << " -> " << it->second;
    while (it != _rangesMap.end()) {
        ss << ", " << it->first << " -> " << it->second;
    }
    return ss.str();
}

BSONObj CollectionMetadata::getMinKey() const {
    BSONObjIterator it(_keyPattern);
    BSONObjBuilder minKeyB;
    while (it.more())
        minKeyB << it.next().fieldName() << MINKEY;
    return minKeyB.obj();
}

BSONObj CollectionMetadata::getMaxKey() const {
    BSONObjIterator it(_keyPattern);
    BSONObjBuilder maxKeyB;
    while (it.more())
        maxKeyB << it.next().fieldName() << MAXKEY;
    return maxKeyB.obj();
}

bool CollectionMetadata::isValid() const {
    if (_shardVersion > _collVersion)
        return false;
    if (_collVersion.majorVersion() == 0)
        return false;
    if (_collVersion.epoch() != _shardVersion.epoch())
        return false;

    if (_shardVersion.majorVersion() > 0) {
        // Must be chunks
        if (_rangesMap.size() == 0 || _chunksMap.size() == 0)
            return false;
    } else {
        // No chunks
        if (_shardVersion.minorVersion() > 0)
            return false;
        if (_rangesMap.size() > 0 || _chunksMap.size() > 0)
            return false;
    }

    return true;
}

bool CollectionMetadata::isValidKey(const BSONObj& key) const {
    BSONObjIterator it(_keyPattern);
    while (it.more()) {
        BSONElement next = it.next();
        if (!key.hasField(next.fieldName()))
            return false;
    }
    return key.nFields() == _keyPattern.nFields();
}

void CollectionMetadata::fillRanges() {
    if (_chunksMap.empty())
        return;

    // Load the chunk information, coallesceing their ranges.  The version for this shard
    // would be the highest version for any of the chunks.
    RangeMap::const_iterator it = _chunksMap.begin();
    BSONObj min, max;
    while (it != _chunksMap.end()) {
        BSONObj currMin = it->first;
        BSONObj currMax = it->second;
        ++it;

        // coalesce the chunk's bounds in ranges if they are adjacent chunks
        if (min.isEmpty()) {
            min = currMin;
            max = currMax;
            continue;
        }
        if (max == currMin) {
            max = currMax;
            continue;
        }

        _rangesMap.insert(make_pair(min, max));

        min = currMin;
        max = currMax;
    }
    dassert(!min.isEmpty());

    _rangesMap.insert(make_pair(min, max));
}

void CollectionMetadata::fillKeyPatternFields() {
    // Parse the shard keys into the states 'keys' and 'keySet' members.
    BSONObjIterator patternIter = _keyPattern.begin();
    while (patternIter.more()) {
        BSONElement current = patternIter.next();

        _keyFields.mutableVector().push_back(new FieldRef);
        FieldRef* const newFieldRef = _keyFields.mutableVector().back();
        newFieldRef->parse(current.fieldNameStringData());
    }
}

}  // namespace mongo
