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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/s/shard_key_index_util.h"

namespace mongo {

namespace {
const IndexDescriptor* _findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                  const CollectionPtr& collection,
                                                  const IndexCatalog* indexCatalog,
                                                  const boost::optional<std::string>& excludeName,
                                                  const BSONObj& shardKey,
                                                  bool requireSingleKey) {
    const IndexDescriptor* best = nullptr;

    auto indexIterator =
        indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (indexIterator->more()) {
        auto indexEntry = indexIterator->next();
        auto indexDescriptor = indexEntry->descriptor();

        if (excludeName && indexDescriptor->indexName() == *excludeName) {
            continue;
        }

        if (isCompatibleWithShardKey(opCtx, collection, indexEntry, shardKey, requireSingleKey)) {
            if (!indexEntry->isMultikey()) {
                return indexDescriptor;
            }

            best = indexDescriptor;
        }
    }

    return best;
}

}  // namespace

bool isCompatibleWithShardKey(OperationContext* opCtx,
                              const CollectionPtr& collection,
                              const IndexCatalogEntry* indexEntry,
                              const BSONObj& shardKey,
                              bool requireSingleKey) {
    auto desc = indexEntry->descriptor();
    bool hasSimpleCollation = desc->collation().isEmpty();

    if (desc->isPartial() || desc->isSparse()) {
        return false;
    }

    if (!shardKey.isPrefixOf(desc->keyPattern(), SimpleBSONElementComparator::kInstance)) {
        return false;
    }

    if (!indexEntry->isMultikey() && hasSimpleCollation) {
        return true;
    }

    if (!requireSingleKey && hasSimpleCollation) {
        return true;
    }

    return false;
}

bool isLastShardKeyIndex(OperationContext* opCtx,
                         const CollectionPtr& collection,
                         const IndexCatalog* indexCatalog,
                         const std::string& indexName,
                         const BSONObj& shardKey) {
    return _findShardKeyPrefixedIndex(opCtx,
                                      collection,
                                      indexCatalog,
                                      indexName,
                                      shardKey,
                                      false /* requireSingleKey */) == nullptr;
}

const IndexDescriptor* findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                 const CollectionPtr& collection,
                                                 const IndexCatalog* indexCatalog,
                                                 const BSONObj& shardKey,
                                                 bool requireSingleKey) {
    return _findShardKeyPrefixedIndex(
        opCtx, collection, indexCatalog, boost::none, shardKey, requireSingleKey);
}

}  // namespace mongo
