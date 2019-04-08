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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_metadata.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

CollectionMetadata::CollectionMetadata(std::shared_ptr<ChunkManager> cm, const ShardId& thisShardId)
    : _cm(std::move(cm)), _thisShardId(thisShardId) {}

BSONObj CollectionMetadata::extractDocumentKey(const BSONObj& doc) const {
    BSONObj key;

    if (isSharded()) {
        auto const& pattern = _cm->getShardKeyPattern();
        key = dotted_path_support::extractElementsBasedOnTemplate(doc, pattern.toBSON());
        if (pattern.hasId()) {
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

void CollectionMetadata::toBSONBasic(BSONObjBuilder& bb) const {
    if (isSharded()) {
        _cm->getVersion().appendLegacyWithField(&bb, "collVersion");
        getShardVersion().appendLegacyWithField(&bb, "shardVersion");
        bb.append("keyPattern", _cm->getShardKeyPattern().toBSON());
    } else {
        ChunkVersion::UNSHARDED().appendLegacyWithField(&bb, "collVersion");
        ChunkVersion::UNSHARDED().appendLegacyWithField(&bb, "shardVersion");
    }
}

std::string CollectionMetadata::toStringBasic() const {
    if (isSharded()) {
        return str::stream() << "collection version: " << _cm->getVersion().toString()
                             << ", shard version: " << getShardVersion().toString();
    } else {
        return "collection version: <unsharded>";
    }
}

RangeMap CollectionMetadata::getChunks() const {
    invariant(isSharded());

    RangeMap chunksMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>());

    for (const auto& chunk : _cm->chunks()) {
        if (chunk.getShardId() != _thisShardId)
            continue;

        chunksMap.emplace_hint(chunksMap.end(), chunk.getMin(), chunk.getMax());
    }

    return chunksMap;
}

bool CollectionMetadata::getNextChunk(const BSONObj& lookupKey, ChunkType* chunk) const {
    invariant(isSharded());

    auto foundIt = _cm->getNextChunkOnShard(lookupKey, _thisShardId);
    if (foundIt.begin() == foundIt.end())
        return false;

    const auto& nextChunk = *foundIt.begin();
    chunk->setMin(nextChunk.getMin());
    chunk->setMax(nextChunk.getMax());
    return true;
}

Status CollectionMetadata::checkChunkIsValid(const ChunkType& chunk) const {
    invariant(isSharded());

    ChunkType existingChunk;

    if (!getNextChunk(chunk.getMin(), &existingChunk)) {
        return {ErrorCodes::StaleShardVersion,
                str::stream() << "Chunk with bounds "
                              << ChunkRange(chunk.getMin(), chunk.getMax()).toString()
                              << " is not owned by this shard."};
    }

    if (existingChunk.getMin().woCompare(chunk.getMin()) ||
        existingChunk.getMax().woCompare(chunk.getMax())) {
        return {ErrorCodes::StaleShardVersion,
                str::stream() << "Unable to find chunk with the exact bounds "
                              << ChunkRange(chunk.getMin(), chunk.getMax()).toString()
                              << " at collection version "
                              << getCollVersion().toString()};
    }

    return Status::OK();
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

    for (const auto& chunk : _cm->chunks()) {
        if (chunk.getShardId() == _thisShardId) {
            BSONArrayBuilder chunkBB(builder->subarrayStart());
            chunkBB.append(chunk.getMin());
            chunkBB.append(chunk.getMax());
            chunkBB.done();
        }
    }
}

}  // namespace mongo
