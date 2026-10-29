// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_indexability.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace plan_cache_detail {
/**
 * Serializes indexability discriminators, appending them to keyBuilder. This function is used
 * during the computation of a query's plan cache key to ensure that two queries with different
 * index eligibilities will have different cache keys.
 */
void encodeIndexability(const MatchExpression* tree,
                        const PlanCacheIndexabilityState& indexabilityState,
                        StringBuilder* keyBuilder);

/**
 * A dispatch tag for the factory functions below.
 */
template <typename KeyType>
struct PlanCacheKeyTag {};

/**
 * Creates a key for the classic plan cache from the canonical query and a single collection.
 */
PlanCacheKey make(const CanonicalQuery& query,
                  const CollectionAcquisition& collection,
                  PlanCacheKeyTag<PlanCacheKey> tag);

/**
 * Similar to above, but for the SBE plan cache key.
 */
sbe::PlanCacheKey make(const CanonicalQuery& query,
                       const CollectionAcquisition& collection,
                       PlanCacheKeyTag<sbe::PlanCacheKey> tag);
}  // namespace plan_cache_detail

namespace plan_cache_key_factory {
/**
 * A factory helper to make a plan cache key of the given type.
 *
 * Note: when requesting an SBE plan cache key, callers must have already established the shard
 * version on the collection (if this is a sharded cluster).
 */
template <typename Key>
Key make(const CanonicalQuery& query, const CollectionAcquisition& collection) {
    return plan_cache_detail::make(query, collection, plan_cache_detail::PlanCacheKeyTag<Key>{});
}

/**
 * Similar to above, a factory helper to make a SBE plan cache key, but used for find queries that
 * might involve multiple collections.
 */
sbe::PlanCacheKey make(const CanonicalQuery& query,
                       const MultipleCollectionAccessor& collections,
                       bool requiresSbeCompatibility = true);

/**
 * Similar to above, a factory helper to make a SBE plan cache key, but used for agg queries that
 * might involve multiple collections.
 */
sbe::PlanCacheKey make(const Pipeline& query, const MultipleCollectionAccessor& collections);

}  // namespace plan_cache_key_factory
}  // namespace mongo
