// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_names.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

class Collection;
class CollectionPtr;
class IndexDescriptor;

/**
 * Returns whether an index type can be used to support a shard key.
 */
bool isAcceptableShardKeyIndexType(IndexType indexType);

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardKeyIndex {
public:
    /**
     * Wraps information pertaining to the 'index' used as the shard key.
     *
     * A clustered index is not tied to an IndexDescriptor whereas all other types of indexes
     * are. Either the 'index' is a clustered index and '_clusteredIndexKeyPattern' is
     * non-empty, or '_indexDescriptor' is non-null and a standard index exists.
     */
    ShardKeyIndex(const IndexCatalogEntry* indexEntry);
    ShardKeyIndex(const ClusteredIndexSpec& clusteredIndexSpec);

    const BSONObj& keyPattern() const;

    const IndexCatalogEntry* indexEntry() const {
        return _indexEntry;
    }

private:
    const IndexCatalogEntry* _indexEntry;

    // Stores the keyPattern when the index is a clustered index and there is no
    // IndexDescriptor. Empty otherwise.
    BSONObj _clusteredIndexKeyPattern;
};

/**
 * Returns true if the given index is compatible with the shard key pattern.
 *
 * If return value is false and errMsg is non-null, the reasons that the existing index is
 * incompatible will be appended to errMsg.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] bool isCompatibleWithShardKey(OperationContext* opCtx,
                                                              const CollectionPtr& collection,
                                                              const IndexCatalogEntry* indexEntry,
                                                              const BSONObj& shardKey,
                                                              bool requireSingleKey,
                                                              std::string* errMsg = nullptr);

/**
 * Returns an index suitable for shard key range scans if it exists.
 *
 * This index:
 * - must be prefixed by 'shardKey', and
 * - must not be a partial index.
 * - must have the simple collation.
 * - must not be hidden.
 *
 * If the parameter 'requireSingleKey' is true, then this index additionally must not be
 * multi-key.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] boost::optional<ShardKeyIndex> findShardKeyPrefixedIndex(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const BSONObj& shardKey,
    bool requireSingleKey,
    std::string* errMsg = nullptr);

/**
 * Returns true if the given index exists and it is the last non-hidden index compatible with the
 * ranged shard key. False otherwise. Hashed indexes are excluded here because users are allowed
 * to drop shard key compatible hashed indexes.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] bool isLastNonHiddenRangedShardKeyIndex(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const std::string& indexName,
    const BSONObj& shardKey);

}  // namespace mongo
