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
     * Instantiates a metadata object, which represents an unsharded collection. This 'isSharded'
     * for this object will return false and it is illegal to use it for filtering.
     */
    CollectionMetadata() = default;

    /**
     * The main way to construct CollectionMetadata is through MetadataLoader or clone() methods.
     *
     * "thisShardId" is the shard identity of this shard for purposes of answering questions like
     * "does this key belong to this shard"?
     */
    CollectionMetadata(std::shared_ptr<ChunkManager> cm, const ShardId& thisShardId);

    /**
     * Returns whether this metadata object represents a sharded or unsharded collection.
     */
    bool isSharded() const {
        return bool(_cm);
    }

    /**
     * Returns the current shard version for the collection or UNSHARDED if it is not sharded.
     */
    ChunkVersion getShardVersion() const {
        return (isSharded() ? _cm->getVersion(_thisShardId) : ChunkVersion::UNSHARDED());
    }

    /**
     * Returns the current collection version or UNSHARDED if it is not sharded.
     */
    ChunkVersion getCollVersion() const {
        return (isSharded() ? _cm->getVersion() : ChunkVersion::UNSHARDED());
    }

    /**
     * Obtains the shard id with which this collection metadata is configured.
     */
    const ShardId& shardId() const {
        invariant(isSharded());
        return _thisShardId;
    }

    /**
     * Returns true if 'key' contains exactly the same fields as the shard key pattern.
     */
    bool isValidKey(const BSONObj& key) const {
        invariant(isSharded());
        return _cm->getShardKeyPattern().isShardKey(key);
    }

    const BSONObj& getKeyPattern() const {
        invariant(isSharded());
        return _cm->getShardKeyPattern().toBSON();
    }

    const std::vector<std::unique_ptr<FieldRef>>& getKeyPatternFields() const {
        invariant(isSharded());
        return _cm->getShardKeyPattern().getKeyPatternFields();
    }

    BSONObj getMinKey() const {
        invariant(isSharded());
        return _cm->getShardKeyPattern().getKeyPattern().globalMin();
    }

    BSONObj getMaxKey() const {
        invariant(isSharded());
        return _cm->getShardKeyPattern().getKeyPattern().globalMax();
    }

    bool uuidMatches(UUID uuid) const {
        invariant(isSharded());
        return _cm->uuidMatches(uuid);
    }

    /**
     * Returns just the shard key fields, if the collection is sharded, and the _id field, from
     * `doc`. Does not alter any field values (e.g. by hashing); values are copied verbatim.
     */
    BSONObj extractDocumentKey(const BSONObj& doc) const;

    /**
     * BSON output of the basic metadata information (chunk and shard version).
     */
    void toBSONBasic(BSONObjBuilder& bb) const;

    /**
     * String output of the collection and shard versions.
     */
    std::string toStringBasic() const;

    //
    // Methods used for orphan filtering and general introspection of the chunks owned by the shard
    //

    ChunkManager* getChunkManager() const {
        invariant(isSharded());
        return _cm.get();
    }

    /**
     * Returns true if the document with the given key belongs to this chunkset. If the key is empty
     * returns false. If key is not a valid shard key, the behaviour is undefined.
     */
    bool keyBelongsToMe(const BSONObj& key) const {
        invariant(isSharded());
        return _cm->keyBelongsToShard(key, _thisShardId);
    }

    /**
     * Given a key 'lookupKey' in the shard key range, get the next chunk which overlaps or is
     * greater than this key.  Returns true if a chunk exists, false otherwise.
     *
     * Passing a key that is not a valid shard key for this range results in undefined behavior.
     */
    bool getNextChunk(const BSONObj& lookupKey, ChunkType* chunk) const;

    /**
     * Validates that the passed-in chunk's bounds exactly match a chunk in the metadata cache.
     */
    Status checkChunkIsValid(const ChunkType& chunk) const;

    /**
     * Returns true if the argument range overlaps any chunk.
     */
    bool rangeOverlapsChunk(const ChunkRange& range) const {
        invariant(isSharded());
        return _cm->rangeOverlapsShard(range, _thisShardId);
    }

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
    boost::optional<ChunkRange> getNextOrphanRange(const RangeMap& receiveMap,
                                                   const BSONObj& lookupKey) const;

    /**
     * Returns all the chunks which are contained on this shard.
     */
    RangeMap getChunks() const;

    /**
     * BSON output of the chunks metadata into a BSONArray
     */
    void toBSONChunks(BSONArrayBuilder* builder) const;

private:
    // The full routing table for the collection or nullptr if the collection is not sharded
    std::shared_ptr<ChunkManager> _cm;

    // The identity of this shard, for the purpose of answering "key belongs to me" queries. If the
    // collection is not sharded (_cm is nullptr), then this value will be empty.
    ShardId _thisShardId;
};

}  // namespace mongo
