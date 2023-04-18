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


#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_metadata.h"

#include <fmt/format.h>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

using namespace fmt::literals;

CollectionMetadata::CollectionMetadata(ChunkManager cm, const ShardId& thisShardId)
    : _cm(std::move(cm)), _thisShardId(thisShardId) {}

bool CollectionMetadata::allowMigrations() const {
    return _cm ? _cm->allowMigrations() : true;
}

boost::optional<ShardKeyPattern> CollectionMetadata::getReshardingKeyIfShouldForwardOps() const {
    if (!isSharded())
        return boost::none;

    const auto& reshardingFields = getReshardingFields();

    // A resharding operation should be taking place, and additionally, the coordinator must
    // be in the states during which the recipient tails the donor's oplog. In these states, the
    // donor annotates each of its oplog entries with the appropriate recipients; thus, checking if
    // the coordinator is within these states is equivalent to checking if the donor should
    // append the resharding recipients.
    if (!reshardingFields)
        return boost::none;

    // Used a switch statement so that the compiler warns anyone who modifies the coordinator
    // states enum.
    switch (reshardingFields.value().getState()) {
        case CoordinatorStateEnum::kUnused:
        case CoordinatorStateEnum::kInitializing:
        case CoordinatorStateEnum::kBlockingWrites:
        case CoordinatorStateEnum::kAborting:
        case CoordinatorStateEnum::kCommitting:
        case CoordinatorStateEnum::kDone:
            return boost::none;
        case CoordinatorStateEnum::kPreparingToDonate:
        case CoordinatorStateEnum::kCloning:
        case CoordinatorStateEnum::kApplying:
            // We will actually return a resharding key for these cases.
            break;
    }

    const auto& donorFields = reshardingFields->getDonorFields();

    // If 'reshardingFields' doesn't contain 'donorFields', then it must contain 'recipientFields',
    // implying that collection represents the target collection in a resharding operation.
    if (!donorFields)
        return boost::none;

    return ShardKeyPattern(donorFields->getReshardingKey());
}

void CollectionMetadata::throwIfReshardingInProgress(NamespaceString const& nss) const {
    if (isSharded()) {
        const auto& reshardingFields = getReshardingFields();
        // Throw if the coordinator is not in states "aborting", "committing", or "done".
        if (reshardingFields && reshardingFields->getState() < CoordinatorStateEnum::kAborting) {
            LOGV2(5277122, "reshardCollection in progress", logAttrs(nss));

            uasserted(ErrorCodes::ReshardCollectionInProgress,
                      "reshardCollection is in progress for namespace " +
                          nss.toStringForErrorMsg());
        }
    }
}

BSONObj CollectionMetadata::extractDocumentKey(const ShardKeyPattern* shardKeyPattern,
                                               const BSONObj& doc) {
    BSONObj key;

    if (shardKeyPattern) {
        key = dotted_path_support::extractElementsBasedOnTemplate(doc, shardKeyPattern->toBSON());
        if (shardKeyPattern->hasId()) {
            return key;
        }
        // else, try to append an _id field from the document.
    }

    if (auto id = doc["_id"]) {
        return key.isEmpty() ? id.wrap() : BSONObjBuilder(std::move(key)).append(id).obj();
    }

    // For legacy documents that lack an _id, use the document itself as its key.
    return doc;
}

BSONObj CollectionMetadata::extractDocumentKey(const BSONObj& doc) const {
    return extractDocumentKey(isSharded() ? &_cm->getShardKeyPattern() : nullptr, doc);
}

std::string CollectionMetadata::toStringBasic() const {
    if (isSharded()) {
        return str::stream() << "collection placement version: " << _cm->getVersion().toString()
                             << ", shard placement version: "
                             << getShardPlacementVersionForLogging().toString();
    } else {
        return "collection placement version: <unsharded>";
    }
}

RangeMap CollectionMetadata::getChunks() const {
    invariant(isSharded());

    RangeMap chunksMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>());

    _cm->forEachChunk([this, &chunksMap](const auto& chunk) {
        if (chunk.getShardId() == _thisShardId)
            chunksMap.emplace_hint(chunksMap.end(), chunk.getMin(), chunk.getMax());

        return true;
    });

    return chunksMap;
}

bool CollectionMetadata::getNextChunk(const BSONObj& lookupKey, ChunkType* chunk) const {
    invariant(isSharded());

    auto nextChunk = _cm->getNextChunkOnShard(lookupKey, _thisShardId);
    if (!nextChunk)
        return false;

    chunk->setMin(nextChunk->getMin());
    chunk->setMax(nextChunk->getMax());

    return true;
}

bool CollectionMetadata::currentShardHasAnyChunks() const {
    invariant(isSharded());
    std::set<ShardId> shards;
    _cm->getAllShardIds(&shards);
    return shards.find(_thisShardId) != shards.end();
}

boost::optional<ChunkRange> CollectionMetadata::getNextOrphanRange(
    const RangeMap& receivingChunks, const BSONObj& origLookupKey) const {
    invariant(isSharded());

    const BSONObj maxKey = getMaxKey();
    BSONObj lookupKey = origLookupKey;

    auto chunksMap = getChunks();

    while (lookupKey.woCompare(maxKey) < 0) {
        using Its = std::pair<RangeMap::const_iterator, RangeMap::const_iterator>;

        const auto patchLookupKey = [&](RangeMap const& map) -> boost::optional<Its> {
            auto lowerIt = map.end(), upperIt = map.end();

            if (!map.empty()) {
                upperIt = map.upper_bound(lookupKey);
                lowerIt = upperIt;
                if (upperIt != map.begin())
                    --lowerIt;
                else
                    lowerIt = map.end();
            }

            // If we overlap, continue after the overlap
            //
            // TODO: Could optimize slightly by finding next non-contiguous chunk
            if (lowerIt != map.end() && lowerIt->second.woCompare(lookupKey) > 0) {
                lookupKey = lowerIt->second;  // note side effect
                return boost::none;
            } else {
                return Its(lowerIt, upperIt);
            }
        };

        boost::optional<Its> chunksIts, pendingIts;
        if (!(chunksIts = patchLookupKey(chunksMap)) ||
            !(pendingIts = patchLookupKey(receivingChunks))) {
            continue;
        }

        BSONObj rangeMin = getMinKey();
        BSONObj rangeMax = maxKey;

        const auto patchArgRange = [&rangeMin, &rangeMax](RangeMap const& map, Its const& its) {
            // We know that the lookup key is not covered by a chunk or pending range, and where the
            // previous chunk and pending chunks are.  Now we fill in the bounds as the closest
            // bounds of the surrounding ranges in both maps.
            const auto& lowerIt = its.first;
            const auto& upperIt = its.second;

            if (lowerIt != map.end() && lowerIt->second.woCompare(rangeMin) > 0) {
                rangeMin = lowerIt->second;
            }

            if (upperIt != map.end() && upperIt->first.woCompare(rangeMax) < 0) {
                rangeMax = upperIt->first;
            }
        };

        patchArgRange(chunksMap, *chunksIts);
        patchArgRange(receivingChunks, *pendingIts);

        return ChunkRange(rangeMin.getOwned(), rangeMax.getOwned());
    }

    return boost::none;
}

void CollectionMetadata::toBSONChunks(BSONArrayBuilder* builder) const {
    if (!isSharded())
        return;

    _cm->forEachChunk([this, &builder](const auto& chunk) {
        if (chunk.getShardId() == _thisShardId) {
            BSONArrayBuilder chunkBB(builder->subarrayStart());
            chunkBB.append(chunk.getMin());
            chunkBB.append(chunk.getMax());
            chunkBB.done();
        }

        return true;
    });
}

}  // namespace mongo
