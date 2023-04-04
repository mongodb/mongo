/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/plan_cache_key_factory.h"

#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/s/operation_sharding_state.h"

namespace mongo {
namespace plan_cache_detail {

void encodeIndexabilityForDiscriminators(const MatchExpression* tree,
                                         const IndexToDiscriminatorMap& discriminators,
                                         StringBuilder* keyBuilder) {

    for (auto&& indexAndDiscriminatorPair : discriminators) {
        *keyBuilder << indexAndDiscriminatorPair.second.isMatchCompatibleWithIndex(tree);
    }
}

void encodeIndexabilityRecursive(const MatchExpression* tree,
                                 const PlanCacheIndexabilityState& indexabilityState,
                                 StringBuilder* keyBuilder) {
    if (!tree->path().empty()) {
        const IndexToDiscriminatorMap& discriminators =
            indexabilityState.getPathDiscriminators(tree->path());
        IndexToDiscriminatorMap wildcardDiscriminators =
            indexabilityState.buildWildcardDiscriminators(tree->path());
        if (!discriminators.empty() || !wildcardDiscriminators.empty()) {
            *keyBuilder << kEncodeDiscriminatorsBegin;
            // For each discriminator on this path, append the character '0' or '1'.
            encodeIndexabilityForDiscriminators(tree, discriminators, keyBuilder);
            encodeIndexabilityForDiscriminators(tree, wildcardDiscriminators, keyBuilder);

            *keyBuilder << kEncodeDiscriminatorsEnd;
        }
    } else if (tree->matchType() == MatchExpression::MatchType::NOT) {
        // If the node is not compatible with any type of index, add a single '0' discriminator
        // here. Otherwise add a '1'.
        *keyBuilder << kEncodeDiscriminatorsBegin;
        *keyBuilder << QueryPlannerIXSelect::logicalNodeMayBeSupportedByAnIndex(tree);
        *keyBuilder << kEncodeDiscriminatorsEnd;
    }

    for (size_t i = 0; i < tree->numChildren(); ++i) {
        encodeIndexabilityRecursive(tree->getChild(i), indexabilityState, keyBuilder);
    }
}

void encodeIndexability(const MatchExpression* tree,
                        const PlanCacheIndexabilityState& indexabilityState,
                        StringBuilder* keyBuilder) {
    // Before encoding the indexability of the leaf MatchExpressions, apply the global
    // discriminators to the expression as a whole. This is for cases such as partial indexes which
    // must discriminate based on the entire query.
    const auto& globalDiscriminators = indexabilityState.getGlobalDiscriminators();
    if (!globalDiscriminators.empty()) {
        *keyBuilder << kEncodeGlobalDiscriminatorsBegin;
        for (auto&& indexAndDiscriminatorPair : globalDiscriminators) {
            *keyBuilder << indexAndDiscriminatorPair.second.isMatchCompatibleWithIndex(tree);
        }
        *keyBuilder << kEncodeGlobalDiscriminatorsEnd;
    }

    encodeIndexabilityRecursive(tree, indexabilityState, keyBuilder);
}

PlanCacheKeyInfo makePlanCacheKeyInfo(CanonicalQuery::QueryShapeString&& shapeString,
                                      const MatchExpression* root,
                                      const CollectionPtr& collection) {

    StringBuilder indexabilityKeyBuilder;
    plan_cache_detail::encodeIndexability(
        root,
        CollectionQueryInfo::get(collection).getPlanCacheIndexabilityState(),
        &indexabilityKeyBuilder);

    return PlanCacheKeyInfo(std::move(shapeString), indexabilityKeyBuilder.str());
}

namespace {
/**
 * Returns the highest index commit timestamp associated with an index on 'collection' that is
 * visible to this operation.
 */
boost::optional<Timestamp> computeNewestVisibleIndexTimestamp(OperationContext* opCtx,
                                                              const CollectionPtr& collection) {
    auto recoveryUnit = opCtx->recoveryUnit();
    auto mySnapshot = recoveryUnit->getPointInTimeReadTimestamp(opCtx).get_value_or(
        recoveryUnit->getCatalogConflictingTimestamp());
    if (mySnapshot.isNull()) {
        return boost::none;
    }

    Timestamp currentNewestVisible = Timestamp::min();

    auto ii = collection->getIndexCatalog()->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (ii->more()) {
        const IndexCatalogEntry* ice = ii->next();
        auto minVisibleSnapshot = ice->getMinimumVisibleSnapshot();
        if (!minVisibleSnapshot) {
            continue;
        }

        if (mySnapshot < *minVisibleSnapshot) {
            continue;
        }

        currentNewestVisible = std::max(currentNewestVisible, *minVisibleSnapshot);
    }

    return currentNewestVisible.isNull() ? boost::optional<Timestamp>{} : currentNewestVisible;
}

sbe::PlanCacheKeyCollectionState computeCollectionState(OperationContext* opCtx,
                                                        const CollectionPtr& collection,
                                                        bool isSecondaryColl) {
    boost::optional<sbe::PlanCacheKeyShardingEpoch> keyShardingEpoch;
    // We don't version secondary collections in the current shard versioning protocol. Also, since
    // currently we only push down $lookup to SBE when secondary collections (and main collection)
    // are unsharded, it's OK to not encode the sharding information here.
    if (!isSecondaryColl) {
        const auto shardVersion{
            OperationShardingState::get(opCtx).getShardVersion(collection->ns())};
        if (shardVersion) {
            keyShardingEpoch =
                sbe::PlanCacheKeyShardingEpoch{shardVersion->placementVersion().epoch(),
                                               shardVersion->placementVersion().getTimestamp()};
        }
    }
    return {collection->uuid(),
            CollectionQueryInfo::get(collection).getPlanCacheInvalidatorVersion(),
            plan_cache_detail::computeNewestVisibleIndexTimestamp(opCtx, collection),
            keyShardingEpoch};
}
}  // namespace

PlanCacheKey make(const CanonicalQuery& query,
                  const CollectionPtr& collection,
                  PlanCacheKeyTag<PlanCacheKey> tag) {
    auto shapeString = canonical_query_encoder::encodeClassic(query);
    return {
        plan_cache_detail::makePlanCacheKeyInfo(std::move(shapeString), query.root(), collection)};
}

sbe::PlanCacheKey make(const CanonicalQuery& query,
                       const CollectionPtr& collection,
                       PlanCacheKeyTag<sbe::PlanCacheKey> tag) {
    return plan_cache_key_factory::make(query, MultipleCollectionAccessor(collection));
}
}  // namespace plan_cache_detail

namespace plan_cache_key_factory {
sbe::PlanCacheKey make(const CanonicalQuery& query, const MultipleCollectionAccessor& collections) {
    OperationContext* opCtx = query.getOpCtx();
    auto mainCollectionState = plan_cache_detail::computeCollectionState(
        opCtx, collections.getMainCollection(), false /* isSecondaryColl */);
    std::vector<sbe::PlanCacheKeyCollectionState> secondaryCollectionStates;
    secondaryCollectionStates.reserve(collections.getSecondaryCollections().size());
    // We always use the collection order saved in MultipleCollectionAccessor to populate the plan
    // cache key, which is ordered by the secondary collection namespaces.
    for (auto& [_, collection] : collections.getSecondaryCollections()) {
        if (collection) {
            secondaryCollectionStates.emplace_back(plan_cache_detail::computeCollectionState(
                opCtx, collection, true /* isSecondaryColl */));
        }
    }

    auto shapeString = canonical_query_encoder::encodeSBE(query);
    return {plan_cache_detail::makePlanCacheKeyInfo(
                std::move(shapeString), query.root(), collections.getMainCollection()),
            std::move(mainCollectionState),
            std::move(secondaryCollectionStates)};
}
}  // namespace plan_cache_key_factory

}  // namespace mongo
