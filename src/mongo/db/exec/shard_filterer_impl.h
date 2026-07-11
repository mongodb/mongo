// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

namespace mongo {

class ShardFiltererImpl : public ShardFilterer {
public:
    ShardFiltererImpl(ScopedCollectionFilter collectionFilter);

    std::unique_ptr<ShardFilterer> clone() const override;

    DocumentBelongsResult documentBelongsToMe(const BSONObj& doc) const override;
    DocumentBelongsResult documentBelongsToMe(const WorkingSetMember& wsm) const;

    bool keyBelongsToMe(const BSONObj& shardKey) const override {
        return _collectionFilter.keyBelongsToMe(shardKey);
    };

    bool isCollectionSharded() const override {
        return _collectionFilter.isSharded();
    }

    ChunkManager::ChunkOwnership nearestOwnedChunk(const BSONObj& key,
                                                   ChunkMap::Direction direction) const override {
        return _collectionFilter.nearestOwnedChunk(key, direction);
    }

    const KeyPattern& getKeyPattern() const override;

    size_t getApproximateSize() const override;

    const ScopedCollectionFilter& getFilter() const {
        return _collectionFilter;
    }

private:
    DocumentBelongsResult keyBelongsToMeHelper(const BSONObj& doc) const;

    ScopedCollectionFilter _collectionFilter;
};
}  // namespace mongo
