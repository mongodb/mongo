/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#define LOGV2_FOR_CATALOG_REFRESH(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                                    \
        ID, DLEVEL, {logv2::LogComponent::kShardingCatalogRefresh}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_manager.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/s/chunk_writes_tracker.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"

namespace mongo {
namespace {

const long unsigned int MaxVectorVerticalDepth = CatalogCacheRefreshVectorVerticalDepth;

bool allElementsAreOfType(BSONType type, const BSONObj& obj) {
    for (auto&& elem : obj) {
        if (elem.type() != type) {
            return false;
        }
    }
    return true;
}

void checkAllElementsAreOfType(BSONType type, const BSONObj& o) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Not all elements of " << o << " are of type " << typeName(type),
            allElementsAreOfType(type, o));
}

void appendChunkTo(std::vector<std::shared_ptr<ChunkInfo>>& chunks,
                   const std::shared_ptr<ChunkInfo>& chunk) {
    if (!chunks.empty() && chunk->getRange().overlaps(chunks.back()->getRange())) {
        if (chunks.back()->getLastmod().isOlderThan(chunk->getLastmod())) {
            chunks.pop_back();
            chunks.push_back(chunk);
        }
    } else { 
        chunks.push_back(chunk);
    }
}

// This function processes the passed in chunks by removing the older versions of any overlapping
// chunks. The resulting chunks must be ordered by the maximum bound and not have any
// overlapping chunks. In order to process the original set of chunks correctly which may have
// chunks from older versions of the map that overlap, this algorithm would need to sort by
// ascending minimum bounds before processing it. However, since we want to take advantage of the
// precomputed KeyString representations of the maximum bounds, this function implements the same
// algorithm by reverse sorting the chunks by the maximum before processing but then must
// reverse the resulting collection before it is returned.
std::vector<std::shared_ptr<ChunkInfo>> flatten(const std::vector<ChunkType>& changedChunks) {
    if (changedChunks.empty())
        return std::vector<std::shared_ptr<ChunkInfo>>();

    std::vector<std::shared_ptr<ChunkInfo>> changedChunkInfos(changedChunks.size());
    std::transform(changedChunks.begin(),
                   changedChunks.end(),
                   changedChunkInfos.begin(),
                   [](const auto& c) { return std::make_shared<ChunkInfo>(c); });

    std::sort(changedChunkInfos.begin(), changedChunkInfos.end(), [](const auto& a, const auto& b) {
        return a->getMaxKeyString() > b->getMaxKeyString();
    });

    std::vector<std::shared_ptr<ChunkInfo>> flattened;
    flattened.reserve(changedChunkInfos.size());
    flattened.push_back(changedChunkInfos[0]);

    
    for (size_t i = 1; i < changedChunkInfos.size(); ++i) {
        appendChunkTo(flattened, changedChunkInfos[i]);
    }

    std::reverse(flattened.begin(), flattened.end());

    return flattened;
}

}  // namespace

void ChunkMap::constructShardVersionMap(const std::vector<std::shared_ptr<ChunkInfo>>& changedChunks) {
    ChunkVector::const_iterator current = changedChunks.cbegin();

    boost::optional<BSONObj> firstMin = boost::none;
    boost::optional<BSONObj> lastMax = boost::none;

    while (current != changedChunks.cend()) {
        const auto& firstChunkInRange = *current;
        const auto& currentRangeShardId = firstChunkInRange->getShardIdAt(boost::none);

        // Tracks the max shard version for the shard on which the current range will reside
        auto shardVersionIt = _shardVersions.find(currentRangeShardId);
        if (shardVersionIt == _shardVersions.end()) {
            shardVersionIt = _shardVersions
                                 .emplace(std::piecewise_construct,
                                          std::forward_as_tuple(currentRangeShardId),
                                          std::forward_as_tuple(_collectionVersion.epoch(),
                                                                _collectionVersion.getTimestamp()))
                                 .first;
        }

        auto& maxShardVersion = shardVersionIt->second.shardVersion;
        
        current =
            std::find_if(current,
                         changedChunks.cend(),
                         [&currentRangeShardId, &maxShardVersion, &shardVersionIt](const auto& currentChunk) {
                             if (currentChunk->getShardIdAt(boost::none) != currentRangeShardId)
                                 return true;

                             if (maxShardVersion.isOlderThan(currentChunk->getLastmod()))
                                 maxShardVersion = currentChunk->getLastmod();
                             shardVersionIt->second.count.addAndFetch(1);
                             return false;
                         });
        const auto rangeLast = *std::prev(current);

        const auto& rangeMin = firstChunkInRange->getMin();
        const auto& rangeMax = rangeLast->getMax();

        // Check the continuity of the chunks map
        if (lastMax && !SimpleBSONObjComparator::kInstance.evaluate(*lastMax == rangeMin)) {
            if (SimpleBSONObjComparator::kInstance.evaluate(*lastMax < rangeMin))
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "Gap exists in the routing table between chunks "
                                        << findIntersectingChunk(*lastMax)->getRange().toString()
                                        << " and " << rangeLast->getRange().toString());
            else
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "Overlap exists in the routing table between chunks "
                                        << findIntersectingChunk(*lastMax)->getRange().toString()
                                        << " and " << rangeLast->getRange().toString());
        }

        if (!firstMin)
            firstMin = rangeMin;

        lastMax = rangeMax;

        // If a shard has chunks it must have a shard version, otherwise we have an invalid chunk
        // somewhere, which should have been caught at chunk load time
        invariant(maxShardVersion.isSet());
    }

    if (!changedChunks.empty()) {
        invariant(!_shardVersions.empty());
        invariant(firstMin.is_initialized());
        invariant(lastMax.is_initialized());

        checkAllElementsAreOfType(MinKey, firstMin.get());
        checkAllElementsAreOfType(MaxKey, lastMax.get());
    }

    return;
}

MapChunkVector& ChunkMap::getMapChunkVector() {
    return _chunkMap;
}

int ChunkMap::totalChunksNum() const {
    int total_chunks = 0;
    for (auto& cursor : _chunkMap) {
        total_chunks += cursor.second->size();
    }
    return total_chunks;
}

void ChunkMap::appendChunk(ChunkVector& chunkMap, const std::shared_ptr<ChunkInfo>& chunk) {
    appendChunkTo(chunkMap, chunk);
    const auto chunkVersion = chunk->getLastmod();
    if (_collectionVersion.isOlderThan(chunkVersion)) {
        _collectionVersion = ChunkVersion(chunkVersion.majorVersion(),
                                          chunkVersion.minorVersion(),
                                          chunkVersion.epoch(),
                                          _collTimestamp);
    }
}

std::shared_ptr<ChunkInfo> ChunkMap::findIntersectingChunk(const BSONObj& shardKey) const {
    return _findIntersectingChunk(shardKey);
}

void validateChunkEpoch(const std::shared_ptr<ChunkInfo>& chunk, const ChunkVersion& version) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Changed chunk " << chunk->toString()
                          << " has epoch different from that of the collection " << version.epoch(),
            version.epoch() == chunk->getLastmod().epoch());
}


void validateChunk(const std::shared_ptr<ChunkInfo>& chunk, const ChunkVersion& version) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Changed chunk " << chunk->toString()
                          << " has epoch different from that of the collection " << version.epoch(),
            version.epoch() == chunk->getLastmod().epoch());

    invariant(version.isOlderOrEqualThan(chunk->getLastmod()));
}

void ChunkMap::makeNew(const ChunkVector& changedChunks) {
    std::shared_ptr<ChunkVector> vectorPtr;
    long unsigned int vectorCount = 0;
    std::string chunkMaxKeyString;
    long unsigned int initVectorSize = getMaxVectorVerticalDepthSize();

    for (const auto& chunk : changedChunks) {
        validateChunkEpoch(chunk, _collectionVersion);
        if (vectorCount == 0) {
            vectorPtr = std::make_shared<ChunkVector>();
            vectorPtr->reserve(initVectorSize); 
        }
        
        appendChunk(*vectorPtr, chunk);
        vectorCount++;
    
        if (vectorCount == initVectorSize) {
            chunkMaxKeyString = chunk->getMaxKeyString();
            _chunkMap.insert(std::make_pair(chunkMaxKeyString, vectorPtr));
            vectorCount = 0;
        }
    }
    
    if (vectorCount < initVectorSize && vectorCount > 0) {
        auto& lastVectorChunk = changedChunks[changedChunks.size() - 1];
        chunkMaxKeyString = lastVectorChunk->getMaxKeyString();
        _chunkMap.insert(std::make_pair(chunkMaxKeyString, vectorPtr));
    }
    
    constructShardVersionMap(changedChunks);

    return;
}

void ChunkMap::minKeyMaxKeyValidateCheck(void) const {
    auto minKeyMapIt = _chunkMap.begin();
    invariant(minKeyMapIt != _chunkMap.end());

    auto maxKeyMapIt = _chunkMap.crbegin();
    invariant(maxKeyMapIt != _chunkMap.crend());

    auto minVector = *(minKeyMapIt->second);
    invariant(minVector.size() > 0);
    auto minKey = minVector[0]->getMin();
    checkAllElementsAreOfType(MinKey, minKey);

    auto maxVector = *(maxKeyMapIt->second);
    invariant(maxVector.size() > 0);
    auto maxKey = maxVector[maxVector.size() -1]->getMax();
    checkAllElementsAreOfType(MaxKey, maxKey);
}

void ChunkMap::chunkRangeValidateCheck(const ChunkVector& changedChunks) {
    for (auto current = changedChunks.cbegin(); current != changedChunks.cend(); current++) {
        if (std::next(current) == changedChunks.cend())
            break;
        
        const auto& nextChunkInRange = *std::next(current);
        const auto& chunkInRange  = *current;

        const auto& firstChunkRangeMax = chunkInRange->getMax();
        const auto& nextChunkInRangeMin = nextChunkInRange->getMin();

        // Check the continuity of the chunks map
        if (!SimpleBSONObjComparator::kInstance.evaluate(firstChunkRangeMax == nextChunkInRangeMin)) {
            if (SimpleBSONObjComparator::kInstance.evaluate(firstChunkRangeMax < firstChunkRangeMax))
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "Gap exists in the routing table between chunks "
                                        << findIntersectingChunk(firstChunkRangeMax)->getRange().toString()
                                        << " and " << nextChunkInRangeMin.toString());
            else
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "Overlap exists in the routing table between chunks "
                                        << findIntersectingChunk(firstChunkRangeMax)->getRange().toString()
                                        << " and " << nextChunkInRangeMin.toString());
        }
    }
    
    return;
}

ShardVersionMap ChunkMap::constructTempShardVersionMapCount(const ChunkVector& chunksVector) {
    ShardVersionMap shardVersions;
    for (const auto& current : chunksVector) { 
        const auto& firstChunkInRange = *current;
        const auto& currentRangeShardId = firstChunkInRange.getShardIdAt(boost::none);

        // Tracks the max shard version for the shard on which the current range will reside
        auto shardVersionIt = shardVersions.find(currentRangeShardId);
        if (shardVersionIt == shardVersions.end()) {
            shardVersionIt = shardVersions
                                .emplace(std::piecewise_construct,
                                        std::forward_as_tuple(currentRangeShardId),
                                        std::forward_as_tuple(_collectionVersion.epoch(),
                                                _collectionVersion.getTimestamp()))
                                .first;
        }

        shardVersionIt->second.count.addAndFetch(1);
    }   

    return shardVersions;
}

void ChunkMap::eraseShardVersionMapElem(ShardVersionMap& oldShardVersionsMap, 
        ShardVersionMap& newShardVersionsMap) {
    for (auto entry = _shardVersions.begin(); entry != _shardVersions.end();) {
        auto newShardVersionsMapCount = 0;
        auto oldShardVersionsMapCount = 0;
        
        auto oldShardVersionIt = oldShardVersionsMap.find(entry->first);
        if (oldShardVersionIt != oldShardVersionsMap.end()) {
            oldShardVersionsMapCount = oldShardVersionIt->second.count.load();
        }

        auto newShardVersionIt = newShardVersionsMap.find(entry->first);
        if (newShardVersionIt != newShardVersionsMap.end()) {
            newShardVersionsMapCount = newShardVersionIt->second.count.load();
        }

        entry->second.count.subtractAndFetch(oldShardVersionsMapCount);
        entry->second.count.addAndFetch(newShardVersionsMapCount);
    
        ++entry;
    }   

    for (auto it = _shardVersions.begin(); it != _shardVersions.end();) {
        auto tmpIt = it;
        ++it;
        if (tmpIt->second.count.load() <= 0) {
            _shardVersions.erase(tmpIt->first); 
        } 
    }
}

void ChunkMap::constructShardVersionsByChunk(const std::shared_ptr<ChunkInfo>& chunk, 
    const OID& epoch, boost::optional<Timestamp> timestamp, bool needIncCount) {
    const auto& currentRangeShardId = chunk->getShardIdAt(boost::none);
    auto shardVersionIt = _shardVersions.find(currentRangeShardId);
    if (shardVersionIt == _shardVersions.end()) {
        shardVersionIt = _shardVersions
                              .emplace(std::piecewise_construct,
                                     std::forward_as_tuple(currentRangeShardId),
                                     std::forward_as_tuple(epoch, timestamp))
                              .first;
    }
    
    auto& maxShardVersion = shardVersionIt->second.shardVersion;
    
    if (maxShardVersion.isOlderThan(chunk->getLastmod())) {
         maxShardVersion = chunk->getLastmod();
    }

    if (needIncCount) {
         shardVersionIt->second.count.addAndFetch(1);
    }
    
    return;
}

void ChunkMap::mergeTowChunkVector(const ChunkVector& changeChunks,
    const ChunkVector& oldVectorChunks, ChunkVector& updateChunk) {
    size_t oldChunkMapIndex = 0;
    size_t changedChunkIndex = 0;

    auto oldTempShardVersionMap = constructTempShardVersionMapCount(oldVectorChunks);
    while (oldChunkMapIndex < oldVectorChunks.size() || changedChunkIndex < changeChunks.size()) {
        if (oldChunkMapIndex >= oldVectorChunks.size()) {
             auto& changedChunk = changeChunks[changedChunkIndex++];
             constructShardVersionsByChunk(changedChunk, _collectionVersion.epoch(), _collectionVersion.getTimestamp(), false);
             appendChunk(updateChunk, changedChunk);
             continue;
        }
    
        if (changedChunkIndex >= changeChunks.size()) {
            appendChunk(updateChunk, oldVectorChunks[oldChunkMapIndex++]);
            continue;
        }
    
        auto overlap = oldVectorChunks[oldChunkMapIndex]->getRange().overlaps(
            changeChunks[changedChunkIndex]->getRange());
    
       if (overlap) {
            auto& changedChunk = changeChunks[changedChunkIndex++];
            auto& chunkInfo = oldVectorChunks[oldChunkMapIndex];
    
            auto bytesInReplacedChunk = chunkInfo->getWritesTracker()->getBytesWritten();
            changedChunk->getWritesTracker()->addBytesWritten(bytesInReplacedChunk);
    
            constructShardVersionsByChunk(changedChunk, _collectionVersion.epoch(), _collectionVersion.getTimestamp(), false);
            appendChunk(updateChunk, changedChunk);
        } else {
            appendChunk(updateChunk, oldVectorChunks[oldChunkMapIndex++]);
        }
    }

    auto newTempShardVersionMap = constructTempShardVersionMapCount(updateChunk);
    eraseShardVersionMapElem(oldTempShardVersionMap, newTempShardVersionMap);

    chunkRangeValidateCheck(updateChunk);
}

bool ChunkMap::mergeOverlapMapVectorChunk(const mongo::BSONObj& min,
                                      const mongo::BSONObj& max, ChunkVector& chunkVector) {
    auto updateFlage = false;
    auto minShardKeyString = ShardKeyPattern::toKeyString(min);
    auto itMin = _chunkMap.upper_bound(minShardKeyString);
    
    auto maxShardKeyString = ShardKeyPattern::toKeyString(max);
    auto itMax = _chunkMap.lower_bound(maxShardKeyString);

    if (itMin == itMax) {
        return updateFlage;
    }

    updateFlage = true;
    ChunkVector tmpVector;
    for (auto it = itMin; it != itMax; it++) {
        auto chunkVector = *(it->second);
        for (auto chunk = chunkVector.begin(); chunk != chunkVector.end(); chunk++) {
            tmpVector.push_back(*chunk);
        }
    }
    _chunkMap.erase(itMin, itMax);

    mergeTowChunkVector(*(itMax->second), tmpVector, chunkVector);

    return updateFlage;
}

void ChunkMap::splitChunkVector(ChunkVector& chunkVector) {
    auto totalVectorSize = chunkVector.size();
    long unsigned int vectorDepthSize = getMaxVectorVerticalDepthSize();
    if (totalVectorSize <= vectorDepthSize) {
        return;
    }

    long unsigned int vectorCount = 0;
    long unsigned int splitTotalVector = totalVectorSize / vectorDepthSize + 1;
    long unsigned int eachVectorSize = totalVectorSize / splitTotalVector + 1;

    std::shared_ptr<ChunkVector> vectorPtr;
    std::string chunkMaxKeyString;
    for (const auto& chunk : chunkVector) {
        if (vectorCount == 0) {
            vectorPtr = std::make_shared<ChunkVector>();
            vectorPtr->reserve(eachVectorSize); 
        }
        vectorCount++;

        appendChunk(*vectorPtr, chunk);
        if (vectorCount == eachVectorSize) {
            chunkMaxKeyString = chunk->getMaxKeyString();
            _chunkMap.insert(std::make_pair(chunkMaxKeyString, vectorPtr));
            vectorCount = 0;
        }
    }
    
    if (vectorCount < eachVectorSize && vectorCount > 0) {
        auto& lastVectorChunk = chunkVector[chunkVector.size() - 1];
        chunkMaxKeyString = lastVectorChunk->getMaxKeyString();
        _chunkMap[chunkMaxKeyString] = vectorPtr;
    }

}
void ChunkMap::mergeChunkMap(const ChunkMap& oldChunkMap, ChunkMap& changeChunkMap) {
    for (const auto& changedIter : changeChunkMap._chunkMap) {
        auto changeChunks = *(changedIter.second);
        
        auto oldIter = oldChunkMap._chunkMap.find(changedIter.first);
        invariant(oldIter != oldChunkMap._chunkMap.end());
        auto oldVectorChunks = *(oldIter->second);

        auto updateIter = _chunkMap.find(changedIter.first);
        invariant(updateIter != _chunkMap.end());
        auto updateChunkVectorPtr = std::make_shared<ChunkVector>();
        updateIter->second = updateChunkVectorPtr;
        updateChunkVectorPtr->reserve(oldVectorChunks.size() + changeChunks.size());
        
        mergeTowChunkVector(changeChunks, oldVectorChunks, *updateChunkVectorPtr);
        
        
        auto& updateChunkVector = *updateChunkVectorPtr;
        invariant(updateChunkVector.size() > 0);
        auto& vectorMin = updateChunkVector[0]->getMin();
        auto vectorSize = updateChunkVector.size();
        auto& vectorMax = updateChunkVector[vectorSize-1]->getMax();

        auto mergeOverlapChunkVectorPtr = std::make_shared<ChunkVector>();
        if (mergeOverlapMapVectorChunk(vectorMin, vectorMax, *mergeOverlapChunkVectorPtr)) {
            auto maxShardKeyString = ShardKeyPattern::toKeyString(vectorMax);
            _chunkMap[maxShardKeyString] = mergeOverlapChunkVectorPtr;
            splitChunkVector(*mergeOverlapChunkVectorPtr);
        } else {
            splitChunkVector(*updateChunkVectorPtr);
        }
    }
}

void ChunkMap::makeUpdated(const ChunkMap& oldChunkMap, 
    const ChunkVector& changedChunks) {
    ChunkMap changeChunkMap(_collectionVersion.epoch(), _collectionVersion.getTimestamp(), MaxVectorVerticalDepth);

    std::shared_ptr<ChunkVector> changeChunkVectorPtr;
    for (const auto& chunk : changedChunks) { 
        validateChunk(chunk, _collectionVersion);
        const auto chunkMaxKeyString = chunk->getMaxKeyString();
        
        auto mapIndex = oldChunkMap._chunkMap.lower_bound(chunkMaxKeyString);
        invariant(mapIndex != oldChunkMap._chunkMap.end());

        auto iter = changeChunkMap._chunkMap.find(mapIndex->first);
        if (iter == changeChunkMap._chunkMap.end()) {
            changeChunkVectorPtr = std::make_shared<ChunkVector>();
            changeChunkMap._chunkMap.insert(std::make_pair(mapIndex->first, changeChunkVectorPtr));
            changeChunkVectorPtr->push_back(chunk);
        } else {
            changeChunkVectorPtr->push_back(chunk);
        }
    }

    mergeChunkMap(oldChunkMap, changeChunkMap);
    minKeyMaxKeyValidateCheck();

    return;
}


void ChunkMap::createMerged(const ChunkMap& oldChunkMap,
    const std::vector<std::shared_ptr<ChunkInfo>>& changedChunks,
    const NamespaceString& nss,
    bool isUpdate) {

    Timer t{};
    if (!isUpdate) {
        makeNew(changedChunks);
    } else {
        makeUpdated(oldChunkMap, changedChunks);
    }

    LOGV2_FOR_CATALOG_REFRESH(24200,
                              0,
                              "create new chunkMap",
                              "namespace"_attr = nss.ns(),
                              "full or incremental"_attr = isUpdate ? "incremental" : "full",
                              "duration"_attr = Milliseconds(t.millis()));
}

std::string ChunkMap::toString() const {
    StringBuilder sb;

    sb << "Chunks:\n";
    for (const auto& mapIt : _chunkMap) {
        auto second = *mapIt.second;
        for (auto it = second.begin(); it != second.end(); it++) {
            sb << "\t" << (*it)->toString() << '\n';  
        }
    }

    sb << "Shard versions:\n";
    for (const auto& entry : _shardVersions) {
        sb << "\t" << entry.first << ": " << entry.second.shardVersion.toString() 
            << ", count:" << entry.second.count.load() << '\n';
    }
 
    sb << "collection versions:" << _collectionVersion.toString() << '\n';

    return sb.str();
}

BSONObj ChunkMap::toBSON() const {
    BSONObjBuilder builder;

    builder.append("startingVersion"_sd, getVersion().toBSON());
    builder.append("chunkCount", static_cast<int64_t>(_chunkMap.size()));

    {
        BSONArrayBuilder arrayBuilder(builder.subarrayStart("chunks"_sd));

        for (const auto& topIt : _chunkMap) {
            for (const auto& secondIt: *topIt.second) {
                arrayBuilder.append(secondIt->toString());
            }
        }
    }

    return builder.obj();
}

std::shared_ptr<ChunkInfo> ChunkMap::_findIntersectingChunk(const BSONObj& shardKey,
                                                                       //??bool isMaxInclusive = true
                                                                       bool isMaxInclusive) const {
    auto shardKeyString = ShardKeyPattern::toKeyString(shardKey);

    const auto it = _chunkMap.upper_bound(shardKeyString);
    if (it == _chunkMap.end())
        return std::shared_ptr<ChunkInfo>();

    const auto itSecond = *it->second;
    ChunkVector::const_iterator vectIt;
    if (!isMaxInclusive) { 
        vectIt = std::lower_bound(itSecond.begin(),
                                itSecond.end(),
                                shardKey,
                                [&shardKeyString](const auto& chunkInfo, const BSONObj& shardKey) {
                                    return chunkInfo->getMaxKeyString() < shardKeyString;
                                });
    } else {
        vectIt = std::upper_bound(itSecond.begin(),
                                itSecond.end(),
                                shardKey,
                                [&shardKeyString](const BSONObj& shardKey, const auto& chunkInfo) {
                                    return shardKeyString < chunkInfo->getMaxKeyString();
                                });
    }

    if (vectIt != itSecond.end()) {
        return *vectIt;
    } else {
        return std::shared_ptr<ChunkInfo>();
    }   
}

ChunkVector::const_iterator ChunkMap::_findIntersectingChunkIterator(const BSONObj& shardKey,
                                                                       std::shared_ptr<ChunkVector> vector,
                                                                       bool isMaxInclusive) const {
    auto shardKeyString = ShardKeyPattern::toKeyString(shardKey);

    if (!isMaxInclusive) { 
        return std::lower_bound(vector->begin(),
                                vector->end(),
                                shardKey,
                                [&shardKeyString](const auto& chunkInfo, const BSONObj& shardKey) {
                                    return chunkInfo->getMaxKeyString() < shardKeyString;
                                });
    } else {
        return std::upper_bound(vector->begin(),
                                vector->end(),
                                shardKey,
                                [&shardKeyString](const BSONObj& shardKey, const auto& chunkInfo) {
                                    return shardKeyString < chunkInfo->getMaxKeyString();
                                });
    }
}


std::pair<MapChunkVector::const_iterator, MapChunkVector::const_iterator>
ChunkMap::_overlappingVectorSlotBounds(const mongo::BSONObj& min,
                                      const mongo::BSONObj& max,
                                      bool isMaxInclusive) const {
    auto minShardKeyString = ShardKeyPattern::toKeyString(min);
    
    const auto itMin = _chunkMap.lower_bound(minShardKeyString);
    const auto itMax = [this, &max, isMaxInclusive]() {
        auto maxShardKeyString = ShardKeyPattern::toKeyString(max);
        auto it = isMaxInclusive ? _chunkMap.upper_bound(maxShardKeyString)
                                 : _chunkMap.lower_bound(maxShardKeyString);
        
        return it == _chunkMap.end() ? it : ++it;
    }();

    return {itMin, itMax};
}

std::pair<ChunkVector::const_iterator, ChunkVector::const_iterator>
ChunkMap::_overlappingBounds(const BSONObj& min, const BSONObj& max, bool isMaxInclusive,
    std::shared_ptr<ChunkVector> vect) const {

    const auto itMin = _findIntersectingChunkIterator(min, vect);
    const auto itMax = [&]() {
        auto it = _findIntersectingChunkIterator(max, vect, isMaxInclusive);

        return it == vect->end() ? it : ++it;
    }();

    return {itMin, itMax};
}

ShardVersionTargetingInfo::ShardVersionTargetingInfo(const OID& epoch,
                                                     const boost::optional<Timestamp>& timestamp)
    : shardVersion(0, 0, epoch, timestamp) {}

RoutingTableHistory::RoutingTableHistory(
    NamespaceString nss,
    boost::optional<UUID> uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    bool allowMigrations,
    ChunkMap chunkMap) 
    : _nss(std::move(nss)),
      _uuid(uuid),
      _shardKeyPattern(shardKeyPattern),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _timeseriesFields(std::move(timeseriesFields)),
      _reshardingFields(std::move(reshardingFields)),
      _allowMigrations(allowMigrations),
      _chunkMap(std::move(chunkMap)),
      _shardVersions(_chunkMap.getShardVersion()) {}
      
void RoutingTableHistory::setShardStale(const ShardId& shardId) {
   if (gEnableFinerGrainedCatalogCacheRefresh) {
      auto it = _shardVersions.find(shardId);
      if (it != _shardVersions.end()) {
          it->second.isStale.store(true);
      }
   }
}

void RoutingTableHistory::setAllShardsRefreshed() {
   if (gEnableFinerGrainedCatalogCacheRefresh) {
      for (auto& [shard, targetingInfo] : _shardVersions) {
          targetingInfo.isStale.store(false);
      }
   }
}


Chunk ChunkManager::findIntersectingChunk(const BSONObj& shardKey,
                                          const BSONObj& collation,
                                          bool bypassIsFieldHashedCheck) const {
    const bool hasSimpleCollation = (collation.isEmpty() && !_rt->optRt->getDefaultCollator()) ||
        SimpleBSONObjComparator::kInstance.evaluate(collation == CollationSpec::kSimpleSpec);
    if (!hasSimpleCollation) {
        for (BSONElement elt : shardKey) {
            // We must assume that if the field is specified as "hashed" in the shard key pattern,
            // then the hash value could have come from a collatable type.
            const bool isFieldHashed =
                (_rt->optRt->getShardKeyPattern().isHashedPattern() &&
                 _rt->optRt->getShardKeyPattern().getHashedField().fieldNameStringData() ==
                     elt.fieldNameStringData());

            // If we want to skip the check in the special case where the _id field is hashed and
            // used as the shard key, set bypassIsFieldHashedCheck. This assumes that a request with
            // a query that contains an _id field can target a specific shard.
            uassert(ErrorCodes::ShardKeyNotFound,
                    str::stream() << "Cannot target single shard due to collation of key "
                                  << elt.fieldNameStringData() << " for namespace "
                                  << _rt->optRt->nss(),
                    !CollationIndexKey::isCollatableType(elt.type()) &&
                        (!isFieldHashed || bypassIsFieldHashedCheck));
        }
    }

    auto chunkInfo = _rt->optRt->findIntersectingChunk(shardKey);

    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Cannot target single shard using key " << shardKey
                          << " for namespace " << _rt->optRt->nss(),
            chunkInfo && chunkInfo->containsKey(shardKey));

    return Chunk(*chunkInfo, _clusterTime);
}

bool ChunkManager::keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const {
    if (shardKey.isEmpty())
        return false;

    auto chunkInfo = _rt->optRt->findIntersectingChunk(shardKey);
    if (!chunkInfo)
        return false;

    invariant(chunkInfo->containsKey(shardKey));

    return chunkInfo->getShardIdAt(_clusterTime) == shardId;
}

void ChunkManager::getShardIdsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                                       const BSONObj& query,
                                       const BSONObj& collation,
                                       std::set<ShardId>* shardIds) const {
    auto findCommand = std::make_unique<FindCommandRequest>(_rt->optRt->nss());
    findCommand->setFilter(query.getOwned());

    if (auto uuid = getUUID())
        expCtx->uuid = uuid;

    if (!collation.isEmpty()) {
        findCommand->setCollation(collation.getOwned());
    } else if (_rt->optRt->getDefaultCollator()) {
        auto defaultCollator = _rt->optRt->getDefaultCollator();
        findCommand->setCollation(defaultCollator->getSpec().toBSON());
        expCtx->setCollator(defaultCollator->clone());
    }

    auto cq = uassertStatusOK(
        CanonicalQuery::canonicalize(expCtx->opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    // Fast path for targeting equalities on the shard key.
    auto shardKeyToFind = _rt->optRt->getShardKeyPattern().extractShardKeyFromQuery(*cq);
    if (!shardKeyToFind.isEmpty()) {
        try {
            auto chunk = findIntersectingChunk(shardKeyToFind, collation);
            shardIds->insert(chunk.getShardId());
            return;
        } catch (const DBException&) {
            // The query uses multiple shards
        }
    }

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    IndexBounds bounds = getIndexBoundsForQuery(_rt->optRt->getShardKeyPattern().toBSON(), *cq);

    // Transforms bounds for each shard key field into full shard key ranges
    // for example :
    //   Key { a : 1, b : 1 }
    //   Bounds { a : [1, 2), b : [3, 4) }
    //   => Ranges { a : 1, b : 3 } => { a : 2, b : 4 }
    BoundList ranges = _rt->optRt->getShardKeyPattern().flattenBounds(bounds);

    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        getShardIdsForRange(it->first /*min*/, it->second /*max*/, shardIds);

        // Once we know we need to visit all shards no need to keep looping.
        // However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->optRt->_shardVersions.size()) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        _rt->optRt->forEachChunk([&](const std::shared_ptr<ChunkInfo>& chunkInfo) {
            shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));
            return false;
        });
    }
}

void ChunkManager::getShardIdsForRange(const BSONObj& min,
                                       const BSONObj& max,
                                       std::set<ShardId>* shardIds) const {
    // If our range is [MinKey, MaxKey], we can simply return all shard ids right away. However,
    // this optimization does not apply when we are reading from a snapshot because _shardVersions
    // contains shards with chunks and is built based on the last refresh. Therefore, it is
    // possible for _shardVersions to have fewer entries if a shard no longer owns chunks when it
    // used to at _clusterTime.
    if (!_clusterTime && allElementsAreOfType(MinKey, min) && allElementsAreOfType(MaxKey, max)) {
        getAllShardIds(shardIds);
        return;
    }

    _rt->optRt->forEachOverlappingChunk(min, max, true, [&](auto& chunkInfo) {
        shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards. However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.auto&  
        //if (!_clusterTime && shardIds->size() == _rt->optRt->_shardVersions.size()) {
        if (!_clusterTime && shardIds->size() == _rt->optRt->_shardVersions.size()) {
            return false;
        }

        return true;
    });
}

bool ChunkManager::rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const {
    bool overlapFound = false;

    _rt->optRt->forEachOverlappingChunk(
        range.getMin(), range.getMax(), false, [&](auto& chunkInfo) {
            if (chunkInfo->getShardIdAt(_clusterTime) == shardId) {
                overlapFound = true;
                return false;
            }

            return true;
        });

    return overlapFound;
}

boost::optional<Chunk> ChunkManager::getNextChunkOnShard(const BSONObj& shardKey,
                                                         const ShardId& shardId) const {
    boost::optional<Chunk> chunk;

    _rt->optRt->forEachChunk(
        [&](auto& chunkInfo) {
            if (chunkInfo->getShardIdAt(_clusterTime) == shardId) {
                chunk.emplace(*chunkInfo, _clusterTime);
                return false;
            }
            return true;
        },
        shardKey);

    return chunk;
}

ShardId ChunkManager::getMinKeyShardIdWithSimpleCollation() const {
    auto minKey = getShardKeyPattern().getKeyPattern().globalMin();
    return findIntersectingChunkWithSimpleCollation(minKey).getShardId();
}

void RoutingTableHistory::getAllShardIds(std::set<ShardId>* all) const {
    std::transform(_shardVersions.begin(),
                   _shardVersions.end(),
                   std::inserter(*all, all->begin()),
                   [](const ShardVersionMap::value_type& pair) { return pair.first; });
}

int RoutingTableHistory::getNShardsOwningChunks() const {
    return _shardVersions.size();
}

IndexBounds ChunkManager::getIndexBoundsForQuery(const BSONObj& key,
                                                 const CanonicalQuery& canonicalQuery) {
    // $text is not allowed in planning since we don't have text index on mongos.
    // TODO: Treat $text query as a no-op in planning on mongos. So with shard key {a: 1},
    //       the query { a: 2, $text: { ... } } will only target to {a: 2}.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::TEXT)) {
        IndexBounds bounds;
        IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
        return bounds;
    }

    // Similarly, ignore GEO_NEAR queries in planning, since we do not have geo indexes on mongos.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::GEO_NEAR)) {
        IndexBounds bounds;
        IndexBoundsBuilder::allValuesBounds(key, &bounds);
        return bounds;
    }

    // Consider shard key as an index
    std::string accessMethod = IndexNames::findPluginName(key);
    dassert(accessMethod == IndexNames::BTREE || accessMethod == IndexNames::HASHED);
    const auto indexType = IndexNames::nameToType(accessMethod);

    // Use query framework to generate index bounds
    QueryPlannerParams plannerParams;
    // Must use "shard key" index
    plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;
    IndexEntry indexEntry(key,
                          indexType,
                          IndexDescriptor::kLatestIndexVersion,
                          // The shard key index cannot be multikey.
                          false,
                          // Empty multikey paths, since the shard key index cannot be multikey.
                          MultikeyPaths{},
                          // Empty multikey path set, since the shard key index cannot be multikey.
                          {},
                          false /* sparse */,
                          false /* unique */,
                          IndexEntry::Identifier{"shardkey"},
                          nullptr /* filterExpr */,
                          BSONObj(),
                          nullptr, /* collator */
                          nullptr /* projExec */);
    plannerParams.indices.push_back(std::move(indexEntry));

    auto plannerResult = QueryPlanner::plan(canonicalQuery, plannerParams);
    if (plannerResult.getStatus().code() != ErrorCodes::NoQueryExecutionPlans) {
        auto solutions = uassertStatusOK(std::move(plannerResult));

        // Pick any solution that has non-trivial IndexBounds. bounds.size() == 0 represents a
        // trivial IndexBounds where none of the fields' values are bounded.
        for (auto&& soln : solutions) {
            IndexBounds bounds = collapseQuerySolution(soln->root());
            if (bounds.size() > 0) {
                return bounds;
            }
        }
    }

    // We cannot plan the query without collection scan, so target to all shards.
    IndexBounds bounds;
    IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
    return bounds;
}

IndexBounds ChunkManager::collapseQuerySolution(const QuerySolutionNode* node) {
    if (node->children.empty()) {
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
        LOGV2_ERROR(23833,
                    "could not generate index bounds on query solution tree: {node}",
                    "node"_attr = redact(node->toString()));
        dassert(false);  // We'd like to know this error in testing.

        // Bail out with all shards in production, since this isn't a fatal error.
        return IndexBounds();
    }

    IndexBounds bounds;

    for (std::vector<QuerySolutionNode*>::const_iterator it = node->children.begin();
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
        if (childBounds.size() == 0) {
            // Got unexpected node in query solution tree
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

//MetadataManager::getActiveMetadata    assertIntersectingChunkHasNotMoved
ChunkManager ChunkManager::makeAtTime(const ChunkManager& cm, Timestamp clusterTime) {
    return ChunkManager(cm.dbPrimary(), cm.dbVersion(), cm._rt, clusterTime);
}

bool ChunkManager::allowMigrations() const {
    if (!_rt->optRt)
        return true;
    return _rt->optRt->allowMigrations();
}

std::string ChunkManager::toString() const {
    return _rt->optRt ? _rt->optRt->toString() : "UNSHARDED";
}

bool RoutingTableHistory::compatibleWith(const RoutingTableHistory& other,
                                         const ShardId& shardName) const {
    // Return true if the shard version is the same in the two chunk managers
    // TODO: This doesn't need to be so strong, just major vs
    return other.getVersion(shardName) == getVersion(shardName);
}

ChunkVersion RoutingTableHistory::_getVersion(const ShardId& shardName,
                                              bool throwOnStaleShard) const {                             
    auto it = _shardVersions.find(shardName);
    if (it == _shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch, timestamp)
        const auto collVersion = _chunkMap.getVersion();
        return ChunkVersion(0, 0, collVersion.epoch(), collVersion.getTimestamp());
    }

    if (throwOnStaleShard && gEnableFinerGrainedCatalogCacheRefresh) {
        uassert(ShardInvalidatedForTargetingInfo(_nss),
                "shard has been marked stale",
                !it->second.isStale.load());
    }

    ChunkVersion chunk(it->second.shardVersion);
    return chunk;
}

ChunkVersion RoutingTableHistory::getVersion(const ShardId& shardName) const {
    return _getVersion(shardName, true);
}

ChunkVersion RoutingTableHistory::getVersionForLogging(const ShardId& shardName) const {
    return _getVersion(shardName, false);
}

std::string RoutingTableHistory::toString() const {
    StringBuilder sb;
    sb << "RoutingTableHistory: " << _nss.ns() << " key: " << _shardKeyPattern.toString() << '\n';

    sb << "Chunks:\n";
    _chunkMap.forEach([&sb](const auto& chunk) {
        sb << "\t" << chunk->toString() << '\n';
        return true;
    });

    sb << "Shard versions:\n";
    for (const auto& entry : _shardVersions) {
        sb << "\t" << entry.first << ": " << entry.second.shardVersion.toString() 
            << ", count:"<< entry.second.count.load() << '\n';
    }
    
    return sb.str();
}

RoutingTableHistory RoutingTableHistory::makeNew(
    NamespaceString nss,
    boost::optional<UUID> uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    OID epoch,
    const boost::optional<Timestamp>& timestamp,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    bool allowMigrations,
    const std::vector<ChunkType>& chunks) {
    return RoutingTableHistory(std::move(nss),
                               std::move(uuid),
                               std::move(shardKeyPattern),
                               std::move(defaultCollator),
                               std::move(unique),
                               std::move(timeseriesFields),
                               boost::none,
                               allowMigrations,
                               ChunkMap{epoch, timestamp, MaxVectorVerticalDepth})
        .makeUpdated(std::move(reshardingFields), allowMigrations, chunks);
}

// Note that any new parameters added to RoutingTableHistory::makeUpdated() must also be added to
// ShardServerCatalogCacheLoader::_getLoaderMetadata() and copied into the persisted metadata when
// it may overlap with the enqueued metadata.
RoutingTableHistory RoutingTableHistory::makeUpdated(  
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    bool allowMigrations,
    const std::vector<ChunkType>& changedChunks) const {
    auto changedChunkInfos = flatten(changedChunks);
    if (numChunks() == 0) {
        ChunkMap updatedChunkMap(getVersion().epoch(), getVersion().getTimestamp(), MaxVectorVerticalDepth);

        updatedChunkMap.createMerged(_chunkMap, changedChunkInfos, _nss, false);

        // Only update the same collection.
        invariant(getVersion().epoch() == updatedChunkMap.getVersion().epoch());

        return RoutingTableHistory(_nss,
                                   _uuid,
                                   getShardKeyPattern().getKeyPattern(),
                                   CollatorInterface::cloneCollator(getDefaultCollator()),
                                   isUnique(),
                                   _timeseriesFields,
                                   std::move(reshardingFields),
                                   allowMigrations,
                                   std::move(updatedChunkMap));
    } 
    
    ChunkMap updatedChunkMap(getChunkMap(), getVersion(), _shardVersions, getVersion().getTimestamp(), MaxVectorVerticalDepth);
    
    updatedChunkMap.createMerged(_chunkMap, changedChunkInfos, _nss, true);
    invariant(getVersion().epoch() == updatedChunkMap.getVersion().epoch());

    return RoutingTableHistory(_nss,
                               _uuid,
                               getShardKeyPattern().getKeyPattern(),
                               CollatorInterface::cloneCollator(getDefaultCollator()),
                               isUnique(),
                               _timeseriesFields,
                               std::move(reshardingFields),
                               allowMigrations,
                               std::move(updatedChunkMap));
}

RoutingTableHistory RoutingTableHistory::makeUpdatedReplacingTimestamp(
    const boost::optional<Timestamp>& timestamp) const {
    invariant(getVersion().getTimestamp().is_initialized() != timestamp.is_initialized());
    long unsigned int vectorDepthSize = _chunkMap.getMaxVectorVerticalDepthSize();

    ChunkMap newMap(getVersion().epoch(), timestamp, vectorDepthSize);
    MapChunkVector& mCV = newMap.getMapChunkVector();
    long unsigned int vectorCount = 0;
    std::shared_ptr<ChunkVector> vectorPtr;
    _chunkMap.forEach([&](const std::shared_ptr<ChunkInfo>& chunkInfo) {
        const auto chunkMaxKeyString = chunkInfo->getMaxKeyString();
        if (vectorCount == 0) {
            vectorPtr = std::make_shared<ChunkVector>();
            vectorPtr->reserve(vectorDepthSize); 
        }

        const ChunkVersion oldVersion = chunkInfo->getLastmod();
        auto newChunk = std::make_shared<ChunkInfo>(chunkInfo->getRange(),
                                                       chunkInfo->getMaxKeyString(),
                                                       chunkInfo->getShardId(),
                                                       ChunkVersion(oldVersion.majorVersion(),
                                                                    oldVersion.minorVersion(),
                                                                    oldVersion.epoch(),
                                                                    timestamp),
                                                       chunkInfo->getHistory(),
                                                       chunkInfo->isJumbo(),
                                                       chunkInfo->getWritesTracker());
        newMap.appendChunk(*vectorPtr, newChunk);
        vectorCount++;

        if (vectorCount == vectorDepthSize) {
            mCV.insert(std::make_pair(chunkMaxKeyString, vectorPtr));
            vectorCount = 0;
        }
        
        newMap.constructShardVersionsByChunk(newChunk, oldVersion.epoch(), timestamp, true);

        return true;
    });
    
    if (vectorCount < vectorDepthSize && vectorCount > 0) {
        auto& vector = *vectorPtr;
        auto& lastVectorChunk = vector[vectorPtr->size() - 1];
        const auto lastChunkMaxKeyString = lastVectorChunk->getMaxKeyString();
        mCV.insert(std::make_pair(lastChunkMaxKeyString, vectorPtr));
    }

    return RoutingTableHistory(_nss,
                               _uuid,
                               getShardKeyPattern().getKeyPattern(),
                               CollatorInterface::cloneCollator(getDefaultCollator()),
                               _unique,
                               _timeseriesFields,
                               _reshardingFields,
                               _allowMigrations,
                               std::move(newMap));
}

AtomicWord<uint64_t> ComparableChunkVersion::_epochDisambiguatingSequenceNumSource{1ULL};
AtomicWord<uint64_t> ComparableChunkVersion::_forcedRefreshSequenceNumSource{1ULL};

ComparableChunkVersion ComparableChunkVersion::makeComparableChunkVersion(
    const ChunkVersion& version) {
    return ComparableChunkVersion(_forcedRefreshSequenceNumSource.load(),
                                  version,
                                  _epochDisambiguatingSequenceNumSource.fetchAndAdd(1));
}

ComparableChunkVersion ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh() {
    return ComparableChunkVersion(_forcedRefreshSequenceNumSource.addAndFetch(2) - 1,
                                  boost::none,
                                  _epochDisambiguatingSequenceNumSource.fetchAndAdd(1));
}

void ComparableChunkVersion::setChunkVersion(const ChunkVersion& version) {
    _chunkVersion = version;
}

BSONObj ComparableChunkVersion::toBSONForLogging() const {
    BSONObjBuilder builder;
    if (_chunkVersion)
        builder.append("chunkVersion"_sd, _chunkVersion->toBSON());
    else
        builder.append("chunkVersion"_sd, "None");

    builder.append("forcedRefreshSequenceNum"_sd, static_cast<int64_t>(_forcedRefreshSequenceNum));
    builder.append("epochDisambiguatingSequenceNum"_sd,
                   static_cast<int64_t>(_epochDisambiguatingSequenceNum));

    return builder.obj();
}

bool ComparableChunkVersion::operator==(const ComparableChunkVersion& other) const {
    if (_forcedRefreshSequenceNum != other._forcedRefreshSequenceNum)
        return false;  // Values created on two sides of a forced refresh sequence number are always
                       // considered different
    if (_forcedRefreshSequenceNum == 0)
        return true;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                      // they are always equal

    // Relying on the boost::optional<ChunkVersion>::operator== comparison
    return _chunkVersion == other._chunkVersion;
}

bool ComparableChunkVersion::operator<(const ComparableChunkVersion& other) const {
    if (_forcedRefreshSequenceNum < other._forcedRefreshSequenceNum)
        return true;  // Values created on two sides of a forced refresh sequence number are always
                      // considered different
    if (_forcedRefreshSequenceNum > other._forcedRefreshSequenceNum)
        return false;  // Values created on two sides of a forced refresh sequence number are always
                       // considered different
    if (_forcedRefreshSequenceNum == 0)
        return false;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                       // they are always equal
    if (_chunkVersion.is_initialized() != other._chunkVersion.is_initialized())
        return _epochDisambiguatingSequenceNum <
            other._epochDisambiguatingSequenceNum;  // One side is not initialised, but the other
                                                    // is, which can only happen if one side is
                                                    // ForForcedRefresh and the other is made from
                                                    // makeComparableChunkVersion. In this case, use
                                                    // the _epochDisambiguatingSequenceNum to see
                                                    // which one is more recent.
    if (!_chunkVersion.is_initialized())
        return _epochDisambiguatingSequenceNum <
            other._epochDisambiguatingSequenceNum;  // Both sides are not initialised, which can
                                                    // only happen if both were created from
                                                    // ForForcedRefresh. In this case, use the
                                                    // _epochDisambiguatingSequenceNum to see which
                                                    // one is more recent.

    const boost::optional<Timestamp> timestamp = _chunkVersion->getTimestamp();
    const boost::optional<Timestamp> otherTimestamp = other._chunkVersion->getTimestamp();
    if (timestamp && otherTimestamp) {
        if (_chunkVersion->isSet() && other._chunkVersion->isSet()) {
            if (*timestamp == *otherTimestamp)
                return _chunkVersion->majorVersion() < other._chunkVersion->majorVersion() ||
                    (_chunkVersion->majorVersion() == other._chunkVersion->majorVersion() &&
                     _chunkVersion->minorVersion() < other._chunkVersion->minorVersion());
            else
                return *timestamp < *otherTimestamp;
        } else if (!_chunkVersion->isSet() && !other._chunkVersion->isSet())
            return false;  // Both sides are the "no chunks on the shard version"
    } else if (sameEpoch(other)) {
        if (_chunkVersion->isSet() && other._chunkVersion->isSet())
            return _chunkVersion->majorVersion() < other._chunkVersion->majorVersion() ||
                (_chunkVersion->majorVersion() == other._chunkVersion->majorVersion() &&
                 _chunkVersion->minorVersion() < other._chunkVersion->minorVersion());
        else if (!_chunkVersion->isSet() && !other._chunkVersion->isSet())
            return false;  // Both sides are the "no chunks on the shard version"
    }

    // If the epochs are different, or if they match, but one of the versions is the "no chunks"
    // version, use the _epochDisambiguatingSequenceNum to disambiguate
    return _epochDisambiguatingSequenceNum < other._epochDisambiguatingSequenceNum;
}

ShardEndpoint::ShardEndpoint(const ShardId& shardName,
                             boost::optional<ChunkVersion> shardVersion,
                             boost::optional<DatabaseVersion> dbVersion)
    : shardName(shardName),
      shardVersion(std::move(shardVersion)),
      databaseVersion(std::move(dbVersion)) {
    if (databaseVersion)
        invariant(shardVersion && *shardVersion == ChunkVersion::UNSHARDED());
    else if (shardVersion)
        invariant(*shardVersion != ChunkVersion::UNSHARDED());
    else
        invariant(shardName == ShardId::kConfigServerId);
}

}  // namespace mongo



