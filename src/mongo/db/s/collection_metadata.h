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

#pragma once

#include "mongo/db/range_arithmetic.h"
#include "mongo/s/chunk_manager.h"

namespace mongo {

/**
 * The collection metadata has metadata information about a collection, in particular the
 * sharding information. It's main goal in life is to be capable of answering if a certain
 * document belongs to it or not. (In some scenarios such as chunk migration, a given
 * document is in a shard but cannot be accessed.)
 *
 * To build a collection from config data, please check the MetadataLoader. The methods
 * here allow building a new incarnation of a collection's metadata based on an existing
 * one (e.g, we're splitting in a given collection.).
 *
 * This class's chunk mapping is immutable once constructed.
 */
class CollectionMetadata {
public:
    /**
     * The main way to construct CollectionMetadata is through MetadataLoader or clone() methods.
     *
     * "thisShardId" is the shard identity of this shard for purposes of answering questions like
     * "does this key belong to this shard"?
     */
    CollectionMetadata(std::shared_ptr<ChunkManager> cm, const ShardId& thisShardId);
    ~CollectionMetadata();

    /**
     * Returns true if 'key' contains exactly the same fields as the shard key pattern.
     */
    bool isValidKey(const BSONObj& key) const {
        return _cm->getShardKeyPattern().isShardKey(key);
    }

    /**
     * Returns true if the document key 'key' belongs to this chunkset. Recall that documents of
     * an in-flight chunk migration may be present and should not be considered part of the
     * collection / chunkset yet. Key must be the full shard key.
     */
    bool keyBelongsToMe(const BSONObj& key) const;

    /**
     * Given a key 'lookupKey' in the shard key range, get the next chunk which overlaps or is
     * greater than this key.  Returns true if a chunk exists, false otherwise.
     *
     * Passing a key that is not a valid shard key for this range results in undefined behavior.
     */
    bool getNextChunk(const BSONObj& lookupKey, ChunkType* chunk) const;

    /**
     * Given a chunk identifying key "chunkMinKey", finds a different chunk if one exists.
     */
    bool getDifferentChunk(const BSONObj& chunkMinKey, ChunkType* differentChunk) const;

    /**
     * Validates that the passed-in chunk's bounds exactly match a chunk in the metadata cache.
     */
    Status checkChunkIsValid(const ChunkType& chunk);

    /**
     * Returns true if the argument range overlaps any chunk.
     */
    bool rangeOverlapsChunk(ChunkRange const& range);

    /**
     * Given a key in the shard key range, get the next range which overlaps or is greater than
     * this key.
     *
     * This allows us to do the following to iterate over all orphan ranges:
     *
     * ChunkRange range;
     * BSONObj lookupKey = metadata->getMinKey();
     * boost::optional<ChunkRange> range;
     * while((range = metadata->getNextOrphanRange(receiveMap, lookupKey))) {
     *     lookupKey = range->maxKey;
     * }
     *
     * @param lookupKey passing a key that does not belong to this metadata is undefined.
     * @param receiveMap is an extra set of chunks not considered orphaned.
     *
     * @return orphanRange the output range. Note that the NS is not set.
     */
    boost::optional<ChunkRange> getNextOrphanRange(RangeMap const& receiveMap,
                                                   BSONObj const& lookupKey) const;

    ChunkVersion getCollVersion() const {
        return _cm->getVersion();
    }

    ChunkVersion getShardVersion() const {
        return _shardVersion;
    }

    const RangeMap& getChunks() const {
        return _chunksMap;
    }

    const BSONObj& getKeyPattern() const {
        return _cm->getShardKeyPattern().toBSON();
    }

    const std::vector<std::unique_ptr<FieldRef>>& getKeyPatternFields() const {
        return _cm->getShardKeyPattern().getKeyPatternFields();
    }

    BSONObj getMinKey() const {
        return _cm->getShardKeyPattern().getKeyPattern().globalMin();
    }

    BSONObj getMaxKey() const {
        return _cm->getShardKeyPattern().getKeyPattern().globalMax();
    }

    std::size_t getNumChunks() const {
        return _chunksMap.size();
    }

    /**
     * BSON output of the basic metadata information (chunk and shard version).
     */
    void toBSONBasic(BSONObjBuilder& bb) const;

    /**
     * BSON output of the chunks metadata into a BSONArray
     */
    void toBSONChunks(BSONArrayBuilder& bb) const;

    /**
     * String output of the collection and shard versions.
     */
    std::string toStringBasic() const;

    std::shared_ptr<ChunkManager> getChunkManager() const {
        return _cm;
    }

    bool uuidMatches(UUID uuid) const {
        return _cm->uuidMatches(uuid);
    }

private:
    /**
     * Builds _rangesMap from the contents of _chunksMap.
     */
    void _buildRangesMap();

    // The full routing table for the collection.
    std::shared_ptr<ChunkManager> _cm;

    // The identity of this shard, for the purpose of answering "key belongs to me" queries.
    ShardId _thisShardId;

    // highest ChunkVersion for which this metadata's information is accurate
    ChunkVersion _shardVersion;

    // Map of chunks tracked by this shard
    RangeMap _chunksMap;

    // A second map from a min key into a range of contiguous chunks. This map is redundant with
    // respect to the contents of _chunkMap but we expect high chunk contiguity, especially in small
    // clusters.
    RangeMap _rangesMap;
};

}  // namespace mongo
