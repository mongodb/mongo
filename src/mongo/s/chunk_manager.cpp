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

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_manager.h"

#include <algorithm>

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

bool overlaps(const ChunkInfo& a, const ChunkInfo& b) {
    // Microbenchmarks results showed that comparing keystrings
    // is more performant than comparing BSONObj
    const auto aMinKeyStr = ShardKeyPattern::toKeyString(a.getMin());
    const auto& aMaxKeyStr = a.getMaxKeyString();
    const auto bMinKeyStr = ShardKeyPattern::toKeyString(b.getMin());
    const auto& bMaxKeyStr = b.getMaxKeyString();

    return aMinKeyStr < bMaxKeyStr && aMaxKeyStr > bMinKeyStr;
}

void checkChunksAreContiguous(const ChunkInfo& left, const ChunkInfo& right) {
    const auto& leftKeyString = left.getMaxKeyString();
    const auto rightKeyString = ShardKeyPattern::toKeyString(right.getMin());
    if (leftKeyString == rightKeyString) {
        return;
    }

    if (SimpleBSONObjComparator::kInstance.evaluate(left.getMax() < right.getMin())) {
        uasserted(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Gap exists in the routing table between chunks "
                                << left.getRange().toString() << " and "
                                << right.getRange().toString());
    } else {
        uasserted(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Overlap exists in the routing table between chunks "
                                << left.getRange().toString() << " and "
                                << right.getRange().toString());
    }

    MONGO_UNREACHABLE;
}

using ChunkVector = ChunkMap::ChunkVector;
using ChunkVectorMap = ChunkMap::ChunkVectorMap;

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
    flattened.emplace_back(std::move(changedChunkInfos[0]));

    for (size_t i = 1; i < changedChunkInfos.size(); ++i) {
        auto& chunk = changedChunkInfos[i];
        if (overlaps(*chunk, *flattened.back())) {
            if (flattened.back()->getLastmod().isOlderThan(chunk->getLastmod())) {
                flattened.pop_back();
                flattened.emplace_back(std::move(chunk));
            }
        } else {
            flattened.emplace_back(std::move(chunk));
        }
    }

    std::reverse(flattened.begin(), flattened.end());

    return flattened;
}

}  // namespace

size_t ChunkMap::size() const {
    size_t totalChunks{0};
    for (const auto& mapIt : _chunkVectorMap) {
        totalChunks += mapIt.second->size();
    }
    return totalChunks;
}

std::shared_ptr<ChunkInfo> ChunkMap::findIntersectingChunk(const BSONObj& shardKey) const {
    const auto shardKeyString = ShardKeyPattern::toKeyString(shardKey);

    const auto it = _chunkVectorMap.upper_bound(shardKeyString);
    if (it == _chunkVectorMap.end()) {
        // upper_bound() will miss the last chunkVector if shardKey is actually the MaxKey,
        // thus we need to check explicitly if shardKey is contained in the last chunk.
        if (const auto& lastChunk = std::prev(_chunkVectorMap.end())->second->back();
            lastChunk->containsKey(shardKey)) {
            return lastChunk;
        } else {
            return {};
        }
    }

    const auto& chunkVector = *(it->second);
    const auto chunkIt = _findIntersectingChunkIterator(
        shardKeyString, chunkVector.begin(), chunkVector.end(), true /*isMaxInclusive*/);

    if (chunkIt == chunkVector.end()) {
        return {};
    }

    return *chunkIt;
}

ChunkMap ChunkMap::createMerged(std::vector<std::shared_ptr<ChunkInfo>> changedChunks) const {
    auto updatedChunkMap = _makeUpdated(std::move(changedChunks));
    tassert(6752900,
            "Chunk map found to be empty after refresh",
            updatedChunkMap._chunkVectorMap.size() &&
                updatedChunkMap._chunkVectorMap.begin()->second->size());
    return updatedChunkMap;
}

void ChunkMap::_commitUpdatedChunkVector(std::shared_ptr<ChunkVector>&& chunkVectorPtr,
                                         bool checkMaxKeyConsistency) {

    invariant(!chunkVectorPtr->empty());

    const auto& vectorMaxKeyString = chunkVectorPtr->back()->getMaxKeyString();
    const auto nextMapIt = _chunkVectorMap.lower_bound(vectorMaxKeyString);

    // Check lower bound is consistent
    if (nextMapIt == _chunkVectorMap.begin()) {
        checkAllElementsAreOfType(MinKey, chunkVectorPtr->front()->getMin());
    } else {
        checkChunksAreContiguous(*(std::prev(nextMapIt)->second->back()),
                                 *(chunkVectorPtr->front()));
    }

    if (checkMaxKeyConsistency) {
        // Check upper bound is consistent
        if (nextMapIt == _chunkVectorMap.end()) {
            checkAllElementsAreOfType(MaxKey, chunkVectorPtr->back()->getMax());
        } else {
            checkChunksAreContiguous(*(chunkVectorPtr->back()), *(nextMapIt->second->front()));
        }
    }

    auto minVectorSize = _maxChunkVectorSize / 2;
    if (chunkVectorPtr->size() < minVectorSize) {
        _mergeAndCommitUpdatedChunkVector(nextMapIt, std::move(chunkVectorPtr));
    } else {
        _splitAndCommitUpdatedChunkVector(nextMapIt, std::move(chunkVectorPtr));
    }
}

void ChunkMap::_mergeAndCommitUpdatedChunkVector(ChunkVectorMap::const_iterator pos,
                                                 std::shared_ptr<ChunkVector>&& smallVectorPtr) {
    if (pos == _chunkVectorMap.begin()) {
        // Vector will be placed at the head of the map,
        // thus there is not previous vector we could merge with
        smallVectorPtr->shrink_to_fit();
        _chunkVectorMap.emplace_hint(
            pos, smallVectorPtr->back()->getMaxKeyString(), std::move(smallVectorPtr));

        return;
    }

    auto prevVectorPtr = _chunkVectorMap.extract(std::prev(pos)).mapped();
    auto mergeVectorPtr = std::make_shared<ChunkVector>();
    mergeVectorPtr->reserve(prevVectorPtr->size() + smallVectorPtr->size());

    // Fill initial part of merged vector with a copy of old vector (prevVectorPtr)
    // Note that the old vector is potentially shared with previous ChunkMap instances,
    // thus we copy rather than moving elements to maintain its integrity.
    mergeVectorPtr->insert(mergeVectorPtr->end(), prevVectorPtr->begin(), prevVectorPtr->end());

    // Fill the rest of merged vector with the small updated vector
    mergeVectorPtr->insert(mergeVectorPtr->end(),
                           std::make_move_iterator(smallVectorPtr->begin()),
                           std::make_move_iterator(smallVectorPtr->end()));

    _chunkVectorMap.emplace_hint(
        pos, mergeVectorPtr->back()->getMaxKeyString(), std::move(mergeVectorPtr));
}

/*
 * Split the given chunk vector into pieces not bigger than _maxChunkVectorSize
 * and add them to the chunkVector map.
 *
 * When chunks can't be divided equally among all generated pieces,
 * this algorithm guarantee that the size difference between all pieces will be minimal and that
 * smaller pieces will be placed at the end.
 */
void ChunkMap::_splitAndCommitUpdatedChunkVector(ChunkVectorMap::const_iterator pos,
                                                 std::shared_ptr<ChunkVector>&& chunkVectorPtr) {
    auto& chunkVector = *chunkVectorPtr;
    const long long totalSize = chunkVector.size();
    const long long numPieces = (totalSize + _maxChunkVectorSize - 1) / _maxChunkVectorSize;
    const long long largePieceSize = (totalSize + numPieces - 1) / numPieces;
    const long long numLargePieces = totalSize % numPieces;
    const long long smallPieceSize = totalSize / numPieces;

    auto lastPos = pos;
    auto chunkIt = chunkVector.end();
    for (int pieceCount = 1; pieceCount < numPieces; pieceCount++) {
        auto tmpVectorPtr = std::make_shared<ChunkVector>();
        auto targetPieceSize =
            (numPieces - pieceCount) < numLargePieces ? largePieceSize : smallPieceSize;
        tmpVectorPtr->insert(tmpVectorPtr->end(),
                             std::make_move_iterator(chunkIt - targetPieceSize),
                             std::make_move_iterator(chunkIt));
        chunkIt -= targetPieceSize;
        lastPos = _chunkVectorMap.emplace_hint(
            lastPos, tmpVectorPtr->back()->getMaxKeyString(), std::move(tmpVectorPtr));
    }

    invariant(std::distance(chunkVector.begin(), chunkIt) == largePieceSize);
    chunkVector.resize(largePieceSize);
    chunkVector.shrink_to_fit();
    _chunkVectorMap.emplace_hint(
        lastPos, chunkVector.back()->getMaxKeyString(), std::move(chunkVectorPtr));
}

void ChunkMap::_updateShardVersionFromDiscardedChunk(const ChunkInfo& chunk) {
    auto shardVersionIt = _shardVersions.find(chunk.getShardId());
    if (shardVersionIt != _shardVersions.end() &&
        shardVersionIt->second.shardVersion == chunk.getLastmod()) {
        _shardVersions.erase(shardVersionIt);
    }
}

void ChunkMap::_updateShardVersionFromUpdateChunk(const ChunkInfo& chunk,
                                                  const ShardVersionMap& oldShardVersions) {
    const auto& newVersion = chunk.getLastmod();
    const auto newValidAfter = [&] {
        auto thisChunkValidAfter = chunk.getHistory().empty()
            ? Timestamp{0, 0}
            : chunk.getHistory().front().getValidAfter();

        auto oldShardVersionIt = oldShardVersions.find(chunk.getShardId());
        auto oldShardValidAfter = oldShardVersionIt == oldShardVersions.end()
            ? Timestamp{0, 0}
            : oldShardVersionIt->second.validAfter;

        return std::max(thisChunkValidAfter, oldShardValidAfter);
    }();

    // Version for this chunk shard got updated
    bool versionUpdated{false};

    auto [shardVersionIt, created] =
        _shardVersions.try_emplace(chunk.getShardId(), newVersion, newValidAfter);

    if (created) {
        // We just created a new entry in the _shardVersions map with latest version and latest
        // valid after.
        versionUpdated = true;
    } else {
        // _shardVersions map already contained an entry for this chunk shard

        // Update version for this shard
        if (shardVersionIt->second.shardVersion.isOlderThan(newVersion)) {
            shardVersionIt->second.shardVersion = newVersion;
            versionUpdated = true;
        }

        // Update validAfter for this shard
        if (newValidAfter > shardVersionIt->second.validAfter) {
            shardVersionIt->second.validAfter = newValidAfter;
        }
    }

    // Update version for the entire collection
    if (versionUpdated && _collectionVersion.isOlderThan(newVersion)) {
        _collectionVersion = ChunkVersion{newVersion.majorVersion(),
                                          newVersion.minorVersion(),
                                          newVersion.epoch(),
                                          _collectionVersion.getTimestamp()};
    }
}

ChunkMap ChunkMap::_makeUpdated(ChunkVector&& updateChunks) const {
    ChunkMap newMap{*this};

    if (updateChunks.empty()) {
        // No updates, just clone the original map
        return newMap;
    }

    std::shared_ptr<ChunkVector> oldVectorPtr;
    ChunkVector::const_iterator oldChunkIt;
    ChunkVector::iterator updateChunkIt;
    ChunkVector::const_iterator updateChunkWrittenBytesIt;
    std::shared_ptr<ChunkVector> newVectorPtr;
    bool lastCommittedIsNew;

    const auto processOldChunk = [&](const std::shared_ptr<ChunkInfo>& nextChunkPtr,
                                     bool discard = false) {
        if (discard) {
            // Discard chunk from oldVector

            while (updateChunkWrittenBytesIt != updateChunks.end() &&
                   overlaps(*nextChunkPtr, **updateChunkWrittenBytesIt)) {
                // Copy writtenBytes to all new overlapping chunks
                (*updateChunkWrittenBytesIt++)
                    ->getWritesTracker()
                    ->addBytesWritten(nextChunkPtr->getWritesTracker()->getBytesWritten());
            }

            newMap._updateShardVersionFromDiscardedChunk(*nextChunkPtr);
            // Since we are discarding the old chunk rather than committing,
            // we do not update `lastCommitedIsNew` flag.
        } else {
            if (!newVectorPtr->empty() && lastCommittedIsNew) {
                checkChunksAreContiguous(*newVectorPtr->back(), *nextChunkPtr);
            }
            lastCommittedIsNew = false;
            newVectorPtr->emplace_back(nextChunkPtr);
        }
    };

    const auto processUpdateChunk = [&](std::shared_ptr<ChunkInfo>&& nextChunkPtr) {
        newMap._updateShardVersionFromUpdateChunk(*nextChunkPtr, _shardVersions);
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Changed chunk " << nextChunkPtr->toString()
                              << " has timestamp different from that of the collection "
                              << _collectionVersion.getTimestamp(),
                nextChunkPtr->getLastmod().getTimestamp() == _collectionVersion.getTimestamp());

        uassert(626840,
                str::stream()
                    << "Changed chunk " << nextChunkPtr->toString()
                    << " doesn't have version that's greater or equal than that of the collection "
                    << _collectionVersion.toString(),
                _collectionVersion.isOlderOrEqualThan(nextChunkPtr->getLastmod()));

        if (!newVectorPtr->empty()) {
            checkChunksAreContiguous(*newVectorPtr->back(), *nextChunkPtr);
        }
        lastCommittedIsNew = true;
        newVectorPtr->emplace_back(std::move(nextChunkPtr));
    };

    const auto processOneChunk = [&] {
        dassert(oldChunkIt != oldVectorPtr->end() || updateChunkIt != updateChunks.end());
        if (updateChunkIt == updateChunks.end()) {
            // no more updates
            processOldChunk(*(oldChunkIt++));
            return;
        }
        if (oldChunkIt == oldVectorPtr->end()) {
            // No more old chunks
            processUpdateChunk(std::move(*(updateChunkIt++)));
            return;
        }

        const auto& oldChunk = **oldChunkIt;
        auto& updateChunk = **updateChunkIt;

        // We have both update and old chunk to peak from
        // If they overlaps we discard the old chunk otherwise we process the one with smaller key
        if (overlaps(updateChunk, oldChunk)) {
            processOldChunk(*(oldChunkIt++), true /* discard */);
            return;
        } else {
            // Ranges do not overlap so we yield the chunk with smaller max key
            if (updateChunk.getMaxKeyString() < oldChunk.getMaxKeyString()) {
                processUpdateChunk(std::move(*(updateChunkIt++)));
                return;
            } else {
                processOldChunk(*(oldChunkIt++));
                return;
            }
        }
    };

    updateChunkIt = updateChunks.begin();
    updateChunkWrittenBytesIt = updateChunkIt;
    // Skip first vectors that were not affected by this update since we don't need to modify them
    auto mapIt = newMap._chunkVectorMap.upper_bound(
        ShardKeyPattern::toKeyString((*updateChunkIt)->getRange().getMin()));
    oldVectorPtr =
        mapIt != newMap._chunkVectorMap.end() ? mapIt->second : std::make_shared<ChunkVector>();
    oldChunkIt = oldVectorPtr->begin();
    // Prepare newVector used as destination of merge sort algorithm
    newVectorPtr = std::make_shared<ChunkVector>();
    newVectorPtr->reserve(mapIt != newMap._chunkVectorMap.end()
                              ? oldVectorPtr->size()
                              : std::min(_maxChunkVectorSize, updateChunks.size()));
    lastCommittedIsNew = false;

    // Iterate until we drained all updates and old vectors
    while (updateChunkIt != updateChunks.end() || mapIt != newMap._chunkVectorMap.end()) {
        processOneChunk();

        // Keep processing chunks until we reach the end of the current old vector
        if (oldChunkIt == oldVectorPtr->end()) {
            if (mapIt == newMap._chunkVectorMap.end()) {
                // Only updates left
                if (newVectorPtr->size() >= _maxChunkVectorSize) {
                    auto checkMaxKeyConsistency = updateChunkIt == updateChunks.end();
                    newMap._commitUpdatedChunkVector(std::move(newVectorPtr),
                                                     checkMaxKeyConsistency);
                    newVectorPtr = std::make_shared<ChunkVector>();
                    // Allocate space only for the remaining updates
                    newVectorPtr->reserve(newVectorPtr->size() +
                                          std::min(_maxChunkVectorSize,
                                                   static_cast<size_t>(std::distance(
                                                       updateChunkIt, updateChunks.end()))));
                }
            } else {
                // drained all chunks from old vector in use,
                // remove old vector from the new map since we are going to replace it.
                auto followingMapIt = newMap._chunkVectorMap.erase(mapIt);

                // Advance the map iterator to the next old vector to update
                mapIt = [&] {
                    if (followingMapIt == newMap._chunkVectorMap.end()) {
                        // No more old vector to process
                        return newMap._chunkVectorMap.end();
                    }

                    if (updateChunkIt == updateChunks.end()) {
                        // No more updates skip all remaining vectors
                        return newMap._chunkVectorMap.end();
                    }

                    if (newVectorPtr->size() < _maxChunkVectorSize / 2) {
                        // New vector is too small, keep accumulating next oldVector
                        return followingMapIt;
                    }

                    // next update doesn't overlap with current old vector so we need to jump
                    // forward to the first overlapping old vector.
                    // This is an optimization to skip vectors that are not affected by any updates.
                    auto nextOvelappingMapIt = newMap._chunkVectorMap.upper_bound(
                        ShardKeyPattern::toKeyString((*updateChunkIt)->getRange().getMin()));
                    invariant(nextOvelappingMapIt != newMap._chunkVectorMap.end());
                    return nextOvelappingMapIt;
                }();

                // Commit chunks accumulated in new vector if
                //  - We are skipping next old vector, thus next old chunk is not adjacent to last
                //    committed chunk
                //  - We already reached maxChunkSize and next update is not adjacent to last
                //    committed chunk
                if (mapIt != followingMapIt ||
                    (newVectorPtr->size() >= _maxChunkVectorSize &&
                     (updateChunkIt == updateChunks.end() ||
                      ShardKeyPattern::toKeyString((*updateChunkIt)->getRange().getMin()) !=
                          newVectorPtr->back()->getMaxKeyString()))) {
                    newMap._commitUpdatedChunkVector(std::move(newVectorPtr), true);
                    newVectorPtr = std::make_shared<ChunkVector>();
                }

                if (mapIt != newMap._chunkVectorMap.end()) {
                    // Update references to oldVector
                    oldVectorPtr = mapIt->second;
                    oldChunkIt = oldVectorPtr->begin();
                    // Reserve space for next chunks,
                    // we cannot know before traversing the next old vector how many chunks will be
                    // added to the new vector, thus this reservation is just best effort.
                    newVectorPtr->reserve(newVectorPtr->size() + oldVectorPtr->size());
                } else {
                    // Only updates left, allocate space only for the remaining updates
                    newVectorPtr->reserve(newVectorPtr->size() +
                                          std::min(_maxChunkVectorSize,
                                                   static_cast<size_t>(std::distance(
                                                       updateChunkIt, updateChunks.end()))));
                }
            }
        }
    }

    if (!newVectorPtr->empty()) {
        newMap._commitUpdatedChunkVector(std::move(newVectorPtr), true);
    }

    return newMap;
}

BSONObj ChunkMap::toBSON() const {
    BSONObjBuilder builder;

    getVersion().serializeToBSON("startingVersion"_sd, &builder);
    builder.append("chunkCount", static_cast<int64_t>(size()));

    {
        BSONArrayBuilder arrayBuilder(builder.subarrayStart("chunks"_sd));
        for (const auto& mapIt : _chunkVectorMap) {
            for (const auto& chunkInfoPtr : *mapIt.second) {
                arrayBuilder.append(chunkInfoPtr->toString());
            }
        }
    }

    return builder.obj();
}

std::string ChunkMap::toString() const {
    StringBuilder sb;

    sb << "Bucket size: " << _maxChunkVectorSize << "\n";
    sb << "Num buckets: " << _chunkVectorMap.size() << "\n";
    sb << "Num chunks: " << size() << "\n";
    sb << "Chunks:\n";
    size_t vectorCount{0};
    for (const auto& mapIt : _chunkVectorMap) {
        sb << "\t vector[" << vectorCount++ << "] key: " << mongo::base64::encode(mapIt.first)
           << ", size: " << mapIt.second->size() << "\n";
        for (const auto& chunkInfoPtr : *mapIt.second) {
            sb << "\t" << chunkInfoPtr->toString() << '\n';
        }
    }

    sb << "Shard versions:\n";
    for (const auto& entry : _shardVersions) {
        sb << "\t" << entry.first << ": " << entry.second.shardVersion.toString() << '\n';
    }

    sb << "Collection version:" << _collectionVersion.toString() << '\n';

    return sb.str();
}

ChunkVector::const_iterator ChunkMap::_findIntersectingChunkIterator(
    const std::string& shardKeyString,
    ChunkVector::const_iterator first,
    ChunkVector::const_iterator last,
    bool isMaxInclusive) const {

    if (!isMaxInclusive) {
        return std::lower_bound(first,
                                last,
                                shardKeyString,
                                [&](const auto& chunkInfo, const std::string& shardKeyString) {
                                    return chunkInfo->getMaxKeyString() < shardKeyString;
                                });
    } else {
        return std::upper_bound(first,
                                last,
                                shardKeyString,
                                [&](const std::string& shardKeyString, const auto& chunkInfo) {
                                    return shardKeyString < chunkInfo->getMaxKeyString();
                                });
    }
}


std::pair<ChunkVectorMap::const_iterator, ChunkVectorMap::const_iterator>
ChunkMap::_overlappingVectorSlotBounds(const std::string& minShardKeyStr,
                                       const std::string& maxShardKeyStr,
                                       bool isMaxInclusive) const {

    const auto itMin = _chunkVectorMap.upper_bound(minShardKeyStr);
    const auto itMax = [&]() {
        auto it = isMaxInclusive ? _chunkVectorMap.upper_bound(maxShardKeyStr)
                                 : _chunkVectorMap.lower_bound(maxShardKeyStr);

        return it == _chunkVectorMap.end() ? it : ++it;
    }();

    return {itMin, itMax};
}

ShardVersionTargetingInfo::ShardVersionTargetingInfo(const OID& epoch, const Timestamp& timestamp)
    : shardVersion(0, 0, epoch, timestamp) {}

RoutingTableHistory::RoutingTableHistory(
    NamespaceString nss,
    UUID uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    boost::optional<uint64_t> maxChunkSizeBytes,
    bool allowMigrations,
    ChunkMap chunkMap)
    : _nss(std::move(nss)),
      _uuid(std::move(uuid)),
      _shardKeyPattern(std::move(shardKeyPattern)),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _timeseriesFields(std::move(timeseriesFields)),
      _reshardingFields(std::move(reshardingFields)),
      _maxChunkSizeBytes(maxChunkSizeBytes),
      _allowMigrations(allowMigrations),
      _chunkMap(std::move(chunkMap)),
      _shardVersions(_chunkMap.getShardVersionsMap()) {}

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
            chunkInfo);

    return Chunk(*chunkInfo, _clusterTime);
}

bool ChunkManager::keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const {
    if (shardKey.isEmpty())
        return false;

    auto chunkInfo = _rt->optRt->findIntersectingChunk(shardKey);
    if (!chunkInfo)
        return false;

    return chunkInfo->getShardIdAt(_clusterTime) == shardId;
}

void ChunkManager::getShardIdsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                                       const BSONObj& query,
                                       const BSONObj& collation,
                                       std::set<ShardId>* shardIds,
                                       bool bypassIsFieldHashedCheck) const {
    auto findCommand = std::make_unique<FindCommandRequest>(_rt->optRt->nss());
    findCommand->setFilter(query.getOwned());

    expCtx->uuid = getUUID();

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
            auto chunk = findIntersectingChunk(shardKeyToFind, collation, bypassIsFieldHashedCheck);
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
        _rt->optRt->forEachChunk([&](const auto& chunkInfo) {
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

    _rt->optRt->forEachOverlappingChunk(min, max, true, [&](const auto& chunkInfo) {
        shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards. However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->optRt->_shardVersions.size()) {
            return false;
        }

        return true;
    });
}

bool ChunkManager::rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const {
    bool overlapFound = false;

    _rt->optRt->forEachOverlappingChunk(
        range.getMin(), range.getMax(), false, [&](const auto& chunkInfo) {
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
        [&](const auto& chunkInfo) {
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
        IndexBoundsBuilder::allValuesBounds(key, &bounds, false);  // [minKey, maxKey]
        return bounds;
    }

    // Similarly, ignore GEO_NEAR queries in planning, since we do not have geo indexes on mongos.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::GEO_NEAR)) {
        // If the GEO_NEAR predicate is a child of AND, remove the GEO_NEAR and continue building
        // bounds. Currently a CanonicalQuery can have at most one GEO_NEAR expression, and only at
        // the top-level, so this check is sufficient.
        auto geoIdx = [](auto root) -> boost::optional<size_t> {
            if (root->matchType() == MatchExpression::AND) {
                for (size_t i = 0; i < root->numChildren(); ++i) {
                    if (MatchExpression::GEO_NEAR == root->getChild(i)->matchType()) {
                        return boost::make_optional(i);
                    }
                }
            }
            return boost::none;
        }(canonicalQuery.root());

        if (!geoIdx) {
            IndexBounds bounds;
            IndexBoundsBuilder::allValuesBounds(key, &bounds, false);
            return bounds;
        }

        canonicalQuery.root()->getChildVector()->erase(
            canonicalQuery.root()->getChildVector()->begin() + geoIdx.get());
    }

    // Consider shard key as an index
    std::string accessMethod = IndexNames::findPluginName(key);
    dassert(accessMethod == IndexNames::BTREE || accessMethod == IndexNames::HASHED);
    const auto indexType = IndexNames::nameToType(accessMethod);

    // Use query framework to generate index bounds
    QueryPlannerParams plannerParams;
    // Must use "shard key" index
    plannerParams.options = QueryPlannerParams::STRICT_NO_TABLE_SCAN;
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

    auto statusWithMultiPlanSolns = QueryPlanner::plan(canonicalQuery, plannerParams);
    if (statusWithMultiPlanSolns.getStatus().code() != ErrorCodes::NoQueryExecutionPlans) {
        auto solutions = uassertStatusOK(std::move(statusWithMultiPlanSolns));

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
    IndexBoundsBuilder::allValuesBounds(key, &bounds, false);  // [minKey, maxKey]
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

ChunkManager ChunkManager::makeAtTime(const ChunkManager& cm, Timestamp clusterTime) {
    return ChunkManager(cm.dbPrimary(), cm.dbVersion(), cm._rt, clusterTime);
}

bool ChunkManager::allowMigrations() const {
    if (!_rt->optRt)
        return true;
    return _rt->optRt->allowMigrations();
}

bool ChunkManager::allowAutoSplit() const {
    const auto maxChunkSize = maxChunkSizeBytes();
    if (!maxChunkSize)
        return true;

    return *maxChunkSize != 0;
}

boost::optional<uint64_t> ChunkManager::maxChunkSizeBytes() const {
    if (!_rt->optRt)
        return boost::none;
    return _rt->optRt->maxChunkSizeBytes();
}

std::string ChunkManager::toString() const {
    return _rt->optRt ? _rt->optRt->toString() : "UNSHARDED";
}

ShardVersionTargetingInfo RoutingTableHistory::_getVersion(const ShardId& shardName,
                                                           bool throwOnStaleShard) const {
    auto it = _shardVersions.find(shardName);
    if (it == _shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch, timestamp)
        const auto collVersion = _chunkMap.getVersion();
        return ShardVersionTargetingInfo(
            ChunkVersion(0, 0, collVersion.epoch(), collVersion.getTimestamp()), Timestamp(0, 0));
    }

    if (throwOnStaleShard && gEnableFinerGrainedCatalogCacheRefresh) {
        uassert(ShardInvalidatedForTargetingInfo(_nss),
                "shard has been marked stale",
                !it->second.isStale.load());
    }

    const auto& shardVersionTargetingInfo = it->second;
    return ShardVersionTargetingInfo(shardVersionTargetingInfo.shardVersion,
                                     shardVersionTargetingInfo.validAfter);
}

std::string RoutingTableHistory::toString() const {
    StringBuilder sb;
    sb << "RoutingTableHistory: " << _nss.ns() << " key: " << _shardKeyPattern.toString() << '\n';

    sb << _chunkMap.toString();

    sb << "Shard versions:\n";
    for (const auto& entry : _shardVersions) {
        sb << "\t" << entry.first << ": " << entry.second.shardVersion.toString() << " @ "
           << entry.second.validAfter.toString() << '\n';
    }

    return sb.str();
}

RoutingTableHistory RoutingTableHistory::makeNew(
    NamespaceString nss,
    UUID uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    OID epoch,
    const Timestamp& timestamp,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    boost::optional<uint64_t> maxChunkSizeBytes,
    bool allowMigrations,
    const std::vector<ChunkType>& chunks) {

    auto changedChunkInfos = flatten(chunks);

    return RoutingTableHistory(
        std::move(nss),
        std::move(uuid),
        std::move(shardKeyPattern),
        std::move(defaultCollator),
        std::move(unique),
        std::move(timeseriesFields),
        std::move(reshardingFields),
        boost::none,
        allowMigrations,
        ChunkMap{epoch, timestamp, static_cast<size_t>(gRoutingTableCacheChunkBucketSize)}
            .createMerged(std::move(changedChunkInfos)));
}

// Note that any new parameters added to RoutingTableHistory::makeUpdated() must also be added to
// ShardServerCatalogCacheLoader::_getLoaderMetadata() and copied into the persisted metadata when
// it may overlap with the enqueued metadata.
RoutingTableHistory RoutingTableHistory::makeUpdated(
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    boost::optional<uint64_t> maxChunkSizeBytes,
    bool allowMigrations,
    const std::vector<ChunkType>& changedChunks) const {

    auto changedChunkInfos = flatten(changedChunks);
    auto chunkMap = _chunkMap.createMerged(std::move(changedChunkInfos));

    // Only update the same collection.
    invariant(getVersion().isSameCollection(chunkMap.getVersion()));

    return RoutingTableHistory(_nss,
                               _uuid,
                               getShardKeyPattern().getKeyPattern(),
                               CollatorInterface::cloneCollator(getDefaultCollator()),
                               isUnique(),
                               std::move(timeseriesFields),
                               std::move(reshardingFields),
                               maxChunkSizeBytes,
                               allowMigrations,
                               std::move(chunkMap));
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

std::string ComparableChunkVersion::toString() const {
    BSONObjBuilder builder;
    if (_chunkVersion)
        _chunkVersion->serializeToBSON("chunkVersion"_sd, &builder);
    else
        builder.append("chunkVersion"_sd, "None");

    builder.append("forcedRefreshSequenceNum"_sd, static_cast<int64_t>(_forcedRefreshSequenceNum));
    builder.append("epochDisambiguatingSequenceNum"_sd,
                   static_cast<int64_t>(_epochDisambiguatingSequenceNum));

    return builder.obj().toString();
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

    if (_chunkVersion->getTimestamp() == other._chunkVersion->getTimestamp()) {
        if (!_chunkVersion->isSet() && !other._chunkVersion->isSet()) {
            return false;
        } else if (_chunkVersion->isSet() && other._chunkVersion->isSet()) {
            return _chunkVersion->majorVersion() < other._chunkVersion->majorVersion() ||
                (_chunkVersion->majorVersion() == other._chunkVersion->majorVersion() &&
                 _chunkVersion->minorVersion() < other._chunkVersion->minorVersion());
        }
    } else if (_chunkVersion->isSet() && other._chunkVersion->isSet()) {
        return _chunkVersion->getTimestamp() < other._chunkVersion->getTimestamp();
    }
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
