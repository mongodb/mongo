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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Interface for doing shard filtering, to be used by both the find and agg execution trees, and
 * slot-based execution.
 */
class ShardFilterer {
public:
    enum class DocumentBelongsResult { kDoesNotBelong, kBelongs, kNoShardKey };

    virtual ~ShardFilterer() = default;

    virtual std::unique_ptr<ShardFilterer> clone() const = 0;

    /**
     * Checks if a shard key is owned by the current node according to the filtering metadata of a
     * sharded collection. This method assumes that the provided shard key is valid (non-empty).
     */
    virtual bool keyBelongsToMe(const BSONObj& key) const = 0;

    /**
     * A higher-level helper that must extract the shard key and then pass the shard key extracted
     * to keyBelongsToMe.
     */
    virtual DocumentBelongsResult documentBelongsToMe(const BSONObj& doc) const = 0;

    /**
     * This method determines if the collection sharded.
     */
    virtual bool isCollectionSharded() const = 0;

    /**
     * Returns a KeyPattern representation of the shard key pattern being used to test membership of
     * the shard key.
     */
    virtual const KeyPattern& getKeyPattern() const = 0;

    virtual size_t getApproximateSize() const = 0;

    virtual ChunkManager::ChunkOwnership nearestOwnedChunk(const BSONObj& key,
                                                           ChunkMap::Direction direction) const {
        MONGO_UNIMPLEMENTED_TASSERT(9246502);
    }
};
}  // namespace mongo
