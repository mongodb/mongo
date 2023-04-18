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

#include "mongo/s/chunk_manager.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

void checkAllElementsAreOfType(BSONType type, const BSONObj& o) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Not all elements of " << o << " are of type " << typeName(type),
            ChunkMap::allElementsAreOfType(type, o));
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

void validateChunkIsNotOlderThan(const std::shared_ptr<ChunkInfo>& chunk,
                                 const ChunkVersion& version) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Changed chunk " << chunk->toString()
                          << " has timestamp different from that of the collection "
                          << version.getTimestamp(),
            version.getTimestamp() == chunk->getLastmod().getTimestamp());

    uassert(626840,
            str::stream()
                << "Changed chunk " << chunk->toString()
                << " doesn't have version that's greater or equal than that of the collection "
                << version.toString(),
            version.isOlderOrEqualThan(chunk->getLastmod()));
}

}  // namespace

ChunkMap::ChunkMap(OID epoch, const Timestamp& timestamp, size_t initialCapacity)
    : _collectionPlacementVersion({epoch, timestamp}, {0, 0}) {
    _chunkMap.reserve(initialCapacity);
}

ShardPlacementVersionMap ChunkMap::constructShardPlacementVersionMap() const {
    ShardPlacementVersionMap placementVersions;
    ChunkVector::const_iterator current = _chunkMap.cbegin();

    boost::optional<BSONObj> firstMin = boost::none;
    boost::optional<BSONObj> lastMax = boost::none;

    while (current != _chunkMap.cend()) {
        const auto& firstChunkInRange = *current;
        const auto& currentRangeShardId = firstChunkInRange->getShardIdAt(boost::none);

        // Tracks the max placement version for the shard on which the current range will reside
        auto placementVersionIt = placementVersions.find(currentRangeShardId);
        if (placementVersionIt == placementVersions.end()) {
            placementVersionIt =
                placementVersions
                    .emplace(std::piecewise_construct,
                             std::forward_as_tuple(currentRangeShardId),
                             std::forward_as_tuple(_collectionPlacementVersion.epoch(),
                                                   _collectionPlacementVersion.getTimestamp()))
                    .first;
        }

        auto& maxPlacementVersion = placementVersionIt->second.placementVersion;

        current =
            std::find_if(current,
                         _chunkMap.cend(),
                         [&currentRangeShardId, &maxPlacementVersion](const auto& currentChunk) {
                             if (currentChunk->getShardIdAt(boost::none) != currentRangeShardId)
                                 return true;

                             if (maxPlacementVersion.isOlderThan(currentChunk->getLastmod()))
                                 maxPlacementVersion = currentChunk->getLastmod();

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

        // If a shard has chunks it must have a placement version, otherwise we have an invalid
        // chunk somewhere, which should have been caught at chunk load time
        invariant(maxPlacementVersion.isSet());
    }

    if (!_chunkMap.empty()) {
        invariant(!placementVersions.empty());
        invariant(firstMin.has_value());
        invariant(lastMax.has_value());

        checkAllElementsAreOfType(MinKey, firstMin.value());
        checkAllElementsAreOfType(MaxKey, lastMax.value());
    }

    return placementVersions;
}

void ChunkMap::appendChunk(const std::shared_ptr<ChunkInfo>& chunk) {
    appendChunkTo(_chunkMap, chunk);
    const auto chunkVersion = chunk->getLastmod();
    if (_collectionPlacementVersion.isOlderThan(chunkVersion)) {
        _collectionPlacementVersion = chunkVersion;
    }
}

std::shared_ptr<ChunkInfo> ChunkMap::findIntersectingChunk(const BSONObj& shardKey) const {
    const auto it = _findIntersectingChunk(shardKey);

    if (it != _chunkMap.end())
        return *it;

    return std::shared_ptr<ChunkInfo>();
}

ChunkMap ChunkMap::createMerged(
    const std::vector<std::shared_ptr<ChunkInfo>>& changedChunks) const {
    size_t chunkMapIndex = 0;
    size_t changedChunkIndex = 0;

    ChunkMap updatedChunkMap(
        getVersion().epoch(), getVersion().getTimestamp(), _chunkMap.size() + changedChunks.size());

    while (chunkMapIndex < _chunkMap.size() || changedChunkIndex < changedChunks.size()) {
        if (chunkMapIndex >= _chunkMap.size()) {
            validateChunkIsNotOlderThan(changedChunks[changedChunkIndex], getVersion());
            updatedChunkMap.appendChunk(changedChunks[changedChunkIndex++]);
            continue;
        }

        if (changedChunkIndex >= changedChunks.size()) {
            updatedChunkMap.appendChunk(_chunkMap[chunkMapIndex++]);
            continue;
        }

        auto overlap = _chunkMap[chunkMapIndex]->getRange().overlaps(
            changedChunks[changedChunkIndex]->getRange());

        if (overlap) {
            auto& changedChunk = changedChunks[changedChunkIndex++];
            validateChunkIsNotOlderThan(changedChunk, getVersion());
            updatedChunkMap.appendChunk(changedChunk);
        } else {
            updatedChunkMap.appendChunk(_chunkMap[chunkMapIndex++]);
        }
    }

    return updatedChunkMap;
}

BSONObj ChunkMap::toBSON() const {
    BSONObjBuilder builder;

    getVersion().serialize("startingVersion"_sd, &builder);
    builder.append("chunkCount", static_cast<int64_t>(_chunkMap.size()));

    {
        BSONArrayBuilder arrayBuilder(builder.subarrayStart("chunks"_sd));
        for (const auto& chunk : _chunkMap) {
            arrayBuilder.append(chunk->toString());
        }
    }

    return builder.obj();
}

bool ChunkMap::allElementsAreOfType(BSONType type, const BSONObj& obj) {
    for (auto&& elem : obj) {
        if (elem.type() != type) {
            return false;
        }
    }
    return true;
}

ChunkMap::ChunkVector::const_iterator ChunkMap::_findIntersectingChunk(const BSONObj& shardKey,
                                                                       bool isMaxInclusive) const {
    auto shardKeyString = ShardKeyPattern::toKeyString(shardKey);

    if (!isMaxInclusive) {
        return std::lower_bound(_chunkMap.begin(),
                                _chunkMap.end(),
                                shardKey,
                                [&shardKeyString](const auto& chunkInfo, const BSONObj& shardKey) {
                                    return chunkInfo->getMaxKeyString() < shardKeyString;
                                });
    } else {
        return std::upper_bound(_chunkMap.begin(),
                                _chunkMap.end(),
                                shardKey,
                                [&shardKeyString](const BSONObj& shardKey, const auto& chunkInfo) {
                                    return shardKeyString < chunkInfo->getMaxKeyString();
                                });
    }
}

std::pair<ChunkMap::ChunkVector::const_iterator, ChunkMap::ChunkVector::const_iterator>
ChunkMap::_overlappingBounds(const BSONObj& min, const BSONObj& max, bool isMaxInclusive) const {
    const auto itMin = _findIntersectingChunk(min);
    const auto itMax = [&]() {
        auto it = _findIntersectingChunk(max, isMaxInclusive);
        return it == _chunkMap.end() ? it : ++it;
    }();

    return {itMin, itMax};
}

PlacementVersionTargetingInfo::PlacementVersionTargetingInfo(const OID& epoch,
                                                             const Timestamp& timestamp)
    : placementVersion({epoch, timestamp}, {0, 0}) {}

RoutingTableHistory::RoutingTableHistory(
    NamespaceString nss,
    UUID uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    bool allowMigrations,
    ChunkMap chunkMap)
    : _nss(std::move(nss)),
      _uuid(std::move(uuid)),
      _shardKeyPattern(std::move(shardKeyPattern)),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _timeseriesFields(std::move(timeseriesFields)),
      _reshardingFields(std::move(reshardingFields)),
      _allowMigrations(allowMigrations),
      _chunkMap(std::move(chunkMap)),
      _placementVersions(_chunkMap.constructShardPlacementVersionMap()) {}

void RoutingTableHistory::setShardStale(const ShardId& shardId) {
    if (gEnableFinerGrainedCatalogCacheRefresh) {
        auto it = _placementVersions.find(shardId);
        if (it != _placementVersions.end()) {
            it->second.isStale.store(true);
        }
    }
}

void RoutingTableHistory::setAllShardsRefreshed() {
    if (gEnableFinerGrainedCatalogCacheRefresh) {
        for (auto& [shard, targetingInfo] : _placementVersions) {
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
                                  << _rt->optRt->nss().toStringForErrorMsg(),
                    !CollationIndexKey::isCollatableType(elt.type()) &&
                        (!isFieldHashed || bypassIsFieldHashedCheck));
        }
    }

    auto chunkInfo = _rt->optRt->findIntersectingChunk(shardKey);

    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Cannot target single shard using key " << shardKey
                          << " for namespace " << _rt->optRt->nss().toStringForErrorMsg(),
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

void ChunkManager::getShardIdsForRange(const BSONObj& min,
                                       const BSONObj& max,
                                       std::set<ShardId>* shardIds,
                                       std::set<ChunkRange>* chunkRanges) const {
    // If our range is [MinKey, MaxKey], we can simply return all shard ids right away. However,
    // this optimization does not apply when we are reading from a snapshot because
    // _placementVersions contains shards with chunks and is built based on the last refresh.
    // Therefore, it is possible for _placementVersions to have fewer entries if a shard no longer
    // owns chunks when it used to at _clusterTime.
    if (!_clusterTime && ChunkMap::allElementsAreOfType(MinKey, min) &&
        ChunkMap::allElementsAreOfType(MaxKey, max)) {
        getAllShardIds(shardIds);
        if (chunkRanges) {
            getAllChunkRanges(chunkRanges);
        }
    }

    _rt->optRt->forEachOverlappingChunk(min, max, true, [&](auto& chunkInfo) {
        shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));
        if (chunkRanges) {
            chunkRanges->insert(chunkInfo->getRange());
        }

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards. However, this optimization does not apply when we are reading from a snapshot
        // because _placementVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _placementVersions to have fewer entries if a
        // shard no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->optRt->_placementVersions.size()) {
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
    boost::optional<Chunk> optChunk;
    forEachChunk(
        [&](const Chunk& chunk) {
            if (chunk.getShardId() == shardId) {
                optChunk.emplace(chunk);
                return false;
            }
            return true;
        },
        shardKey);

    return optChunk;
}

ShardId ChunkManager::getMinKeyShardIdWithSimpleCollation() const {
    auto minKey = getShardKeyPattern().getKeyPattern().globalMin();
    return findIntersectingChunkWithSimpleCollation(minKey).getShardId();
}

void RoutingTableHistory::getAllShardIds(std::set<ShardId>* all) const {
    invariant(all->empty());

    std::transform(_placementVersions.begin(),
                   _placementVersions.end(),
                   std::inserter(*all, all->begin()),
                   [](const ShardPlacementVersionMap::value_type& pair) { return pair.first; });
}

void RoutingTableHistory::getAllChunkRanges(std::set<ChunkRange>* all) const {
    forEachChunk([&](const std::shared_ptr<ChunkInfo>& chunkInfo) {
        all->insert(chunkInfo->getRange());
        return true;
    });
}

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
    // Return true if the placement version is the same in the two chunk managers
    // TODO: This doesn't need to be so strong, just major vs
    return other.getVersion(shardName) == getVersion(shardName);
}

ChunkVersion RoutingTableHistory::_getVersion(const ShardId& shardName,
                                              bool throwOnStaleShard) const {
    auto it = _placementVersions.find(shardName);
    if (it == _placementVersions.end()) {
        // Shards without explicitly tracked placement versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch, timestamp)
        const auto collPlacementVersion = _chunkMap.getVersion();
        return ChunkVersion({collPlacementVersion.epoch(), collPlacementVersion.getTimestamp()},
                            {0, 0});
    }

    if (throwOnStaleShard && gEnableFinerGrainedCatalogCacheRefresh) {
        uassert(ShardInvalidatedForTargetingInfo(_nss),
                "shard has been marked stale",
                !it->second.isStale.load());
    }

    return it->second.placementVersion;
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

    sb << "Shard placement versions:\n";
    for (const auto& entry : _placementVersions) {
        sb << "\t" << entry.first << ": " << entry.second.placementVersion.toString() << '\n';
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
    bool allowMigrations,
    const std::vector<ChunkType>& chunks) {

    auto changedChunkInfos = flatten(chunks);
    return RoutingTableHistory(std::move(nss),
                               std::move(uuid),
                               std::move(shardKeyPattern),
                               std::move(defaultCollator),
                               std::move(unique),
                               std::move(timeseriesFields),
                               std::move(reshardingFields),
                               allowMigrations,
                               ChunkMap{epoch, timestamp}.createMerged(changedChunkInfos));
}

// Note that any new parameters added to RoutingTableHistory::makeUpdated() must also be added to
// ShardServerCatalogCacheLoader::_getLoaderMetadata() and copied into the persisted metadata when
// it may overlap with the enqueued metadata.
RoutingTableHistory RoutingTableHistory::makeUpdated(
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<TypeCollectionReshardingFields> reshardingFields,
    bool allowMigrations,
    const std::vector<ChunkType>& changedChunks) const {

    auto changedChunkInfos = flatten(changedChunks);
    auto chunkMap = _chunkMap.createMerged(changedChunkInfos);

    // Only update the same collection.
    invariant(getVersion().isSameCollection(chunkMap.getVersion()));

    return RoutingTableHistory(_nss,
                               _uuid,
                               getShardKeyPattern().getKeyPattern(),
                               CollatorInterface::cloneCollator(getDefaultCollator()),
                               isUnique(),
                               std::move(timeseriesFields),
                               std::move(reshardingFields),
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
        _chunkVersion->serialize("chunkVersion"_sd, &builder);
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
    if (_chunkVersion.has_value() != other._chunkVersion.has_value())
        return _epochDisambiguatingSequenceNum <
            other._epochDisambiguatingSequenceNum;  // One side is not initialised, but the other
                                                    // is, which can only happen if one side is
                                                    // ForForcedRefresh and the other is made from
                                                    // makeComparableChunkVersion. In this case, use
                                                    // the _epochDisambiguatingSequenceNum to see
                                                    // which one is more recent.
    if (!_chunkVersion.has_value())
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
                             boost::optional<ShardVersion> shardVersionParam,
                             boost::optional<DatabaseVersion> dbVersionParam)
    : shardName(shardName),
      shardVersion(std::move(shardVersionParam)),
      databaseVersion(std::move(dbVersionParam)) {
    if (databaseVersion)
        invariant(shardVersion && *shardVersion == ShardVersion::UNSHARDED());
    else if (shardVersion)
        invariant(*shardVersion != ShardVersion::UNSHARDED());
    else
        invariant(shardName == ShardId::kConfigServerId);
}

bool EndpointComp::operator()(const ShardEndpoint* endpointA,
                              const ShardEndpoint* endpointB) const {
    const int shardNameDiff = endpointA->shardName.compare(endpointB->shardName);
    if (shardNameDiff)
        return shardNameDiff < 0;

    if (endpointA->shardVersion && endpointB->shardVersion) {
        const int epochDiff = endpointA->shardVersion->placementVersion().epoch().compare(
            endpointB->shardVersion->placementVersion().epoch());
        if (epochDiff)
            return epochDiff < 0;

        const int shardVersionDiff = endpointA->shardVersion->placementVersion().toLong() -
            endpointB->shardVersion->placementVersion().toLong();
        if (shardVersionDiff)
            return shardVersionDiff < 0;
    } else if (!endpointA->shardVersion && !endpointB->shardVersion) {
        // TODO (SERVER-51070): Can only happen if the destination is the config server
        return false;
    } else {
        // TODO (SERVER-51070): Can only happen if the destination is the config server
        return !endpointA->shardVersion && endpointB->shardVersion;
    }

    if (endpointA->databaseVersion && endpointB->databaseVersion) {
        const int uuidDiff =
            endpointA->databaseVersion->getUuid().compare(endpointB->databaseVersion->getUuid());
        if (uuidDiff)
            return uuidDiff < 0;

        return endpointA->databaseVersion->getLastMod() < endpointB->databaseVersion->getLastMod();
    } else if (!endpointA->databaseVersion && !endpointB->databaseVersion) {
        return false;
    } else {
        return !endpointA->databaseVersion && endpointB->databaseVersion;
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
