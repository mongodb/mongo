// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/sharding_environment/sharding_api_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/logv2/log_severity_suppressor.h"
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

    const IndexCatalogEntry* best = nullptr;

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
                return ShardKeyIndex(indexEntry);
            }

            best = indexEntry;
        }
    }

    if (best != nullptr) {
        return ShardKeyIndex(best);
    }

    return boost::none;
}

}  // namespace

ShardKeyIndex::ShardKeyIndex(const IndexCatalogEntry* indexEntry) : _indexEntry(indexEntry) {
    tassert(6012300,
            "The indexEntry for ShardKeyIndex(const IndexCatalogEntry* indexEntry) must not "
            "be a nullptr",
            indexEntry != nullptr);
}

ShardKeyIndex::ShardKeyIndex(const ClusteredIndexSpec& clusteredIndexSpec)
    : _indexEntry(nullptr), _clusteredIndexKeyPattern(clusteredIndexSpec.getKey().getOwned()) {}

const BSONObj& ShardKeyIndex::keyPattern() const {
    if (_indexEntry != nullptr) {
        return _indexEntry->descriptor()->keyPattern();
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
    const int kErrorWildcard = 0x20;
    int reasons = 0;

    auto desc = indexEntry->descriptor();
    bool hasSimpleCollation = desc->collation().isEmpty();

    if (desc->isPartial()) {
        reasons |= kErrorPartial;
    }

    if (desc->behavesAsSparse()) {
        reasons |= kErrorSparse;
        if (desc->getIndexType() == IndexType::INDEX_WILDCARD) {
            reasons |= kErrorWildcard;
        }
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

    if (reasons != 0) {
        std::string errors = "Index " + desc->indexName() + " cannot be used for sharding because:";
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
        if (reasons & kErrorWildcard) {
            errors += " Index key is a wildcard index.";
        }

        if (errMsg) {
            if (!errMsg->empty()) {
                *errMsg += "\n";
            }
            *errMsg += errors;
        }

        // TODO (SERVER-112793) Remove metrics reporting for compound wildcard indexes prefixed by
        // the shard key once v9.0 branches out.
        if (reasons & kErrorWildcard && !(reasons & kErrorNotPrefix)) {
            ShardingStatistics::get(opCtx)
                .countHitsOfCompoundWildcardIndexesWithShardKeyPrefix.addAndFetch(1);
            if (enableCompoundWildcardIndexLog.load()) {
                static logv2::SeveritySuppressor logSeverity{
                    Hours{1}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(5)};
                LOGV2_DEBUG(11279201,
                            logSeverity().toInt(),
                            "Found a compound wildcard index prefixed by the shard key",
                            "index"_attr = desc->keyPattern(),
                            "indexName"_attr = desc->indexName(),
                            "shardKey"_attr = shardKey,
                            "nss"_attr = collection.get()->ns());
            }
        }
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

            opCtx, collection, index, shardKey, false /* requireSingleKey */)) {
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
