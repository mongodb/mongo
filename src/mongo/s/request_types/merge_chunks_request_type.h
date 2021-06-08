/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

/**
 * Provides support for parsing and serialization of arguments to the config server mergeChunks
 * command.
 */
class MergeChunksRequest {
public:
    MergeChunksRequest(NamespaceString nss,
                       ShardId shardId,
                       UUID collUUID,
                       ChunkRange chunkRange,
                       boost::optional<Timestamp> validAfter);
    /**
     * Parses the provided BSON content as the internal _configsvrCommitChunksMerge command
     * and if it contains the correct types, constructs a MergeChunksRequest object from it.
     *
     * {
     *   _configsvrCommitChunksMerge: <NamespaceString nss>,
     *   collUUID: <UUID>,
     *   chunkRage: <ChunkRange [minKey, maxKey)>,
     *   shard: <string shard>
     * }
     */
    static StatusWith<MergeChunksRequest> parseFromConfigCommand(const BSONObj& cmdObj);

    /**
     * Creates a BSONObjBuilder and uses it to create and return a BSONObj from this
     * MergeChunksRequest instance. Calls appendAsConfigCommand and tacks on the passed-in
     * writeConcern.
     */
    BSONObj toConfigCommandBSON(const BSONObj& writeConcern);

    /**
     * Creates a serialized BSONObj of the internal _configsvCommitChunksMerge command
     * from this MergeChunksRequest instance.
     */
    void appendAsConfigCommand(BSONObjBuilder* cmdBuilder);

    const NamespaceString& getNamespace() const {
        return _nss;
    }

    const UUID& getCollectionUUID() const {
        return _collectionUUID;
    }

    const ChunkRange& getChunkRange() const {
        return _chunkRange;
    }

    const ShardId& getShardId() const {
        return _shardId;
    }

    const boost::optional<Timestamp>& getValidAfter() const {
        return _validAfter;
    }

private:
    NamespaceString _nss;

    UUID _collectionUUID;

    ChunkRange _chunkRange;

    ShardId _shardId;

    boost::optional<Timestamp> _validAfter;
};

}  // namespace mongo
