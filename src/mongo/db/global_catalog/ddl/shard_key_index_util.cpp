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

#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

boost::optional<ShardKeyIndex> findShardKeyPrefixedIndex(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexCatalog* indexCatalog,
    const boost::optional<std::string>& excludeName,
    const BSONObj& shardKey,
    bool requireSingleKey,
    std::string* errMsg = nullptr) {
    if (collection->isClustered() &&
        clustered_util::matchesClusterKey(shardKey, collection->getClusteredInfo())) {
        auto clusteredIndexSpec = collection->getClusteredInfo()->getIndexSpec();
        return ShardKeyIndex(clusteredIndexSpec);
    }

    const IndexDescriptor* best = nullptr;

    auto indexIterator = indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    while (indexIterator->more()) {
        auto indexEntry = indexIterator->next();
        auto indexDescriptor = indexEntry->descriptor();

        if (excludeName && indexDescriptor->indexName() == *excludeName) {
            continue;
        }

        if (indexDescriptor->hidden()) {
            continue;
        }

        if (isCompatibleWithShardKey(
                opCtx, collection, indexEntry, shardKey, requireSingleKey, errMsg)) {
            if (!indexEntry->isMultikey(opCtx, collection)) {
                return ShardKeyIndex(indexDescriptor);
            }

            best = indexDescriptor;
        }
    }

    if (best != nullptr) {
        return ShardKeyIndex(best);
    }

    return boost::none;
}

}  // namespace

ShardKeyIndex::ShardKeyIndex(const IndexDescriptor* indexDescriptor)
    : _indexDescriptor(indexDescriptor) {
    tassert(6012300,
            "The indexDescriptor for ShardKeyIndex(const IndexDescriptor* indexDescripto) must not "
            "be a nullptr",
            indexDescriptor != nullptr);
}

ShardKeyIndex::ShardKeyIndex(const ClusteredIndexSpec& clusteredIndexSpec)
    : _indexDescriptor(nullptr),
      _clusteredIndexKeyPattern(clusteredIndexSpec.getKey().getOwned()) {}

const BSONObj& ShardKeyIndex::keyPattern() const {
    if (_indexDescriptor != nullptr) {
        return _indexDescriptor->keyPattern();
    }
    return _clusteredIndexKeyPattern;
}

bool isCompatibleWithShardKey(OperationContext* opCtx,
                              const CollectionPtr& collection,
                              const IndexCatalogEntry* indexEntry,
                              const BSONObj& shardKey,
                              bool requireSingleKey,
                              std::string* errMsg) {
    // Return a descriptive error for each index that shares a prefix with shardKey but
    // cannot be used for sharding.
    const int kErrorPartial = 0x01;
    const int kErrorSparse = 0x02;
    const int kErrorMultikey = 0x04;
    const int kErrorCollation = 0x08;
    const int kErrorNotPrefix = 0x10;
    int reasons = 0;

    auto desc = indexEntry->descriptor();
    bool hasSimpleCollation = desc->collation().isEmpty();

    if (desc->isPartial()) {
        reasons |= kErrorPartial;
    }

    if (desc->isSparse()) {
        reasons |= kErrorSparse;
    }

    if (!shardKey.isPrefixOf(desc->keyPattern(), SimpleBSONElementComparator::kInstance)) {
        reasons |= kErrorNotPrefix;
    }

    if (reasons == 0) {  // that is, not partial index, not sparse, and not prefix, then:
        if (!indexEntry->isMultikey(opCtx, collection)) {
            if (hasSimpleCollation) {
                return true;
            }
        } else {
            reasons |= kErrorMultikey;
        }
        if (!requireSingleKey && hasSimpleCollation) {
            return true;
        }
    }

    if (!hasSimpleCollation) {
        reasons |= kErrorCollation;
    }

    if (errMsg && reasons != 0) {
        std::string errors = "Index " + indexEntry->descriptor()->indexName() +
            " cannot be used for sharding because:";
        if (reasons & kErrorPartial) {
            errors += " Index key is partial.";
        }
        if (reasons & kErrorSparse) {
            errors += " Index key is sparse.";
        }
        if (reasons & kErrorMultikey) {
            errors += " Index key is multikey.";
        }
        if (reasons & kErrorCollation) {
            errors += " Index has a non-simple collation.";
        }
        if (reasons & kErrorNotPrefix) {
            errors += " Shard key is not a prefix of index key.";
        }
        if (!errMsg->empty()) {
            *errMsg += "\n";
        }
        *errMsg += errors;
    }
    return false;
}

bool isLastNonHiddenRangedShardKeyIndex(OperationContext* opCtx,
                                        const CollectionPtr& collection,
                                        const std::string& indexName,
                                        const BSONObj& shardKey) {
    const auto index = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    if (!index ||
        !isCompatibleWithShardKey(
            opCtx, collection, index->getEntry(), shardKey, false /* requireSingleKey */)) {
        return false;
    }

    // Users are allowed to drop hashed shard key indexes.
    if (ShardKeyPattern(shardKey).isHashedPattern()) {
        return false;
    }

    return !findShardKeyPrefixedIndex(opCtx,
                                      collection,
                                      collection->getIndexCatalog(),
                                      indexName,
                                      shardKey,
                                      true /* requireSingleKey */)
                .has_value();
}

boost::optional<ShardKeyIndex> findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                         const CollectionPtr& collection,
                                                         const BSONObj& shardKey,
                                                         bool requireSingleKey,
                                                         std::string* errMsg) {
    return findShardKeyPrefixedIndex(opCtx,
                                     collection,
                                     collection->getIndexCatalog(),
                                     boost::none,
                                     shardKey,
                                     requireSingleKey,
                                     errMsg);
}

}  // namespace mongo
