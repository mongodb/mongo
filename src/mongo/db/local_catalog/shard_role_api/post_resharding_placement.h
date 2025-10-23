/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/sharding_environment/shard_id.h"

namespace mongo {

/**
 * Represents the post resharding placement for a collection undergoing resharding at acquisition
 * time. It is based on the given temporary resharding collection chunk manager, which cannot change
 * and therefore it cannot be stale.
 */
class PostReshardingCollectionPlacement {
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

private:
    boost::optional<ShardKeyPattern> _reshardingKeyPattern;
    boost::optional<ChunkManager> _tmpReshardingCollectionChunkManager;
};
}  // namespace mongo
