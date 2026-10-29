// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

namespace mongo {

// TODO: SERVER-127295 Remove this class after we stop using resharding fields.
/**
 * Represents the post resharding placement for a collection undergoing resharding at acquisition
 * time. It is based on the given temporary resharding collection chunk manager, which cannot change
 * and therefore it cannot be stale.
 */
class [[MONGO_MOD_PUBLIC]] PostReshardingCollectionPlacement {
public:
    PostReshardingCollectionPlacement(OperationContext* opCtx,
                                      const ScopedCollectionDescription& collectionDescription);
    // Not copiable
    PostReshardingCollectionPlacement(const PostReshardingCollectionPlacement&) = delete;
    PostReshardingCollectionPlacement& operator=(const PostReshardingCollectionPlacement&) = delete;

    // Movable
    PostReshardingCollectionPlacement(PostReshardingCollectionPlacement&&) = default;
    PostReshardingCollectionPlacement& operator=(PostReshardingCollectionPlacement&&) = default;

    // Returns the post-resharding placement for the document
    const ShardId& getReshardingDestinedRecipient(const BSONObj& fullDocument) const;

    // Extract the resharding key from the input document
    BSONObj extractReshardingKeyFromDocument(const BSONObj& fullDocument) const;

    // Returns the post-resharding placement given the extracted shard key.
    const ShardId& getReshardingDestinedRecipientFromShardKey(const BSONObj& reshardingKey) const;

private:
    void _checkIsValid() const;

    boost::optional<ShardKeyPattern> _reshardingKeyPattern;
    boost::optional<ChunkManager> _tmpReshardingCollectionChunkManager;
    std::string _invalidReason;
};

}  // namespace mongo
