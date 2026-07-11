// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

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
