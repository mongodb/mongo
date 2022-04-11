/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/index_catalog.h"

namespace mongo {

class Collection;
class CollectionPtr;

class IndexDescriptor;

class ShardKeyIndex {
public:
    /**
     * Wraps information pertaining to the 'index' used as the shard key.
     *
     * A clustered index is not tied to an IndexDescriptor whereas all other types of indexes
     * are. Either the 'index' is a clustered index and '_clusteredIndexKeyPattern' is
     * non-empty, or '_indexDescriptor' is non-null and a standard index exists.
     */
    ShardKeyIndex(const IndexDescriptor* indexDescriptor);
    ShardKeyIndex(const ClusteredIndexSpec& clusteredIndexSpec);

    const BSONObj& keyPattern() const;
    const IndexDescriptor* descriptor() const {
        return _indexDescriptor;
    }

private:
    const IndexDescriptor* _indexDescriptor;

    // Stores the keyPattern when the index is a clustered index and there is no
    // IndexDescriptor. Empty otherwise.
    BSONObj _clusteredIndexKeyPattern;
};

/**
 * Returns true if the given index is compatible with the shard key pattern.
 */
bool isCompatibleWithShardKey(OperationContext* opCtx,
                              const CollectionPtr& collection,
                              const IndexCatalogEntry* indexEntry,
                              const BSONObj& shardKey,
                              bool requireSingleKey);

/**
 * Returns an index suitable for shard key range scans if it exists.
 *
 * This index:
 * - must be prefixed by 'shardKey', and
 * - must not be a partial index.
 * - must have the simple collation.
 *
 * If the parameter 'requireSingleKey' is true, then this index additionally must not be
 * multi-key.
 */
boost::optional<ShardKeyIndex> findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                         const CollectionPtr& collection,
                                                         const IndexCatalog* indexCatalog,
                                                         const BSONObj& shardKey,
                                                         bool requireSingleKey);

/**
 * Returns true if the given index name is the last remaining index that is compatible with the
 * shard key index.
 */
bool isLastShardKeyIndex(OperationContext* opCtx,
                         const CollectionPtr& collection,
                         const IndexCatalog* indexCatalog,
                         const std::string& indexName,
                         const BSONObj& shardKey);

}  // namespace mongo
