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

#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"

#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/plan_cache/plan_cache_key_info.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
                                 StringBuilder* keyBuilder,
                                 StringData parentPath = {}) {
    std::string fullPath;
    // Accumulate the path components from any ancestors with partial paths (eg. $elemMatch) through
    // the tree to the leaves. Leaf expressions as children of these partial-path expressions will
    // have an empty path and would otherwise fail to include the discriminators in the key.
    StringData path;
    if (!tree->path().empty()) {
        // This expression has a path component. Either use it, or append it to the parent path.
        if (parentPath.empty()) {
            path = tree->path();
        } else {
            fullPath = std::string{parentPath} + "." + tree->path();
            path = fullPath;
        }
    } else {
        path = parentPath;
    }

    // Check for $not first. We don't need to look at 'path' here; if it is non-empty, then
    // we would have checked the relevant path discriminators while vising an ancestor node.
    if (tree->matchType() == MatchExpression::MatchType::NOT) {
        // If the node is not compatible with any type of index, add a single '0' discriminator
        // here. Otherwise add a '1'.
        *keyBuilder << kEncodeDiscriminatorsBegin;
        *keyBuilder << QueryPlannerIXSelect::logicalNodeMayBeSupportedByAnIndex(tree);
        *keyBuilder << kEncodeDiscriminatorsEnd;
    } else if (!path.empty() && tree->getCategory() != MatchExpression::MatchCategory::kLogical) {
        // Skip checking the discriminators for logical nodes like $and/$or. These are not leaf
        // nodes and don't have paths, so they would never affect the indexability.
        const IndexToDiscriminatorMap& discriminators =
            indexabilityState.getPathDiscriminators(path);
        IndexToDiscriminatorMap wildcardDiscriminators =
            indexabilityState.buildWildcardDiscriminators(path);
        if (!discriminators.empty() || !wildcardDiscriminators.empty()) {
            *keyBuilder << kEncodeDiscriminatorsBegin;
            // For each discriminator on this path, append the character '0' or '1'.
            encodeIndexabilityForDiscriminators(tree, discriminators, keyBuilder);
            encodeIndexabilityForDiscriminators(tree, wildcardDiscriminators, keyBuilder);

            *keyBuilder << kEncodeDiscriminatorsEnd;
        }
    }

    for (size_t i = 0; i < tree->numChildren(); ++i) {
        encodeIndexabilityRecursive(tree->getChild(i), indexabilityState, keyBuilder, path);
    }
}

void encodePartialIndexDiscriminatorHelper(
    const MatchExpression* tree,
    CompositeIndexabilityDiscriminator partialIndexDiscriminator,
    bool inNegationOrElemMatchObj,
    StringBuilder* keyBuilder) {
    // At this point, we know the subexpression is not a subset of the partial filter. Mark this in
    // discriminator.
    *keyBuilder << false;
    inNegationOrElemMatchObj |= Indexability::nodeIsNegationOrElemMatchObj(tree);
    const bool isOrExpression = tree->matchType() == MatchExpression::OR;
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        const auto curChild = tree->getChild(i);
        // If a subexpression is an $or that is not under $elemMatch, $not, or $nor, then mark it as
        // eligible, just like the planner does.
        if (!inNegationOrElemMatchObj && isOrExpression &&
            partialIndexDiscriminator.isMatchCompatibleWithIndex(curChild)) {
            *keyBuilder << true;
            continue;
        }
        // Recursively encode the query's children. There may be a subexpression deeper in the tree
        // that is eligible for the partial index.
        encodePartialIndexDiscriminatorHelper(
            curChild, partialIndexDiscriminator, inNegationOrElemMatchObj, keyBuilder);
    }
}

// Encode partial index discriminator using the same algorithm that is used in
// 'QueryPlannerIXSelect::stripInvalidAssignmentsToPartialIndexRoot()'. This is to ensure that the
// plan cache key agrees with the decisions that the planner makes around index eligibility.
void encodePartialIndexDiscriminator(const MatchExpression* tree,
                                     CompositeIndexabilityDiscriminator partialIndexDiscriminator,
                                     StringBuilder* keyBuilder) {
    // If the query is a subset of the partial filter, then we are done.
    if (partialIndexDiscriminator.isMatchCompatibleWithIndex(tree)) {
        *keyBuilder << true;
        return;
    }
    // Otherwise, walk the query and check if subexpressions are subsets of the partial filter and
    // encode the subexpressions' eligibility in the discriminator. This matches the planner's
    // ability to use a partial index for a subexpression of a query even when the original query is
    // not a subset of the partial filter expression.
    encodePartialIndexDiscriminatorHelper(tree,
                                          partialIndexDiscriminator,
                                          Indexability::nodeIsNegationOrElemMatchObj(tree),
                                          keyBuilder);
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
            encodePartialIndexDiscriminator(tree, indexAndDiscriminatorPair.second, keyBuilder);
        }
        *keyBuilder << kEncodeGlobalDiscriminatorsEnd;
    }

    encodeIndexabilityRecursive(tree, indexabilityState, keyBuilder);
}

PlanCacheKeyInfo makePlanCacheKeyInfo(CanonicalQuery::QueryShapeString&& shapeString,
                                      const MatchExpression* root,
                                      const CollectionPtr& collection,
                                      const query_settings::QuerySettings& querySettings) {

    StringBuilder indexabilityKeyBuilder;
    plan_cache_detail::encodeIndexability(
        root,
        CollectionQueryInfo::get(collection).getPlanCacheIndexabilityState(),
        &indexabilityKeyBuilder);

    return PlanCacheKeyInfo(std::move(shapeString), indexabilityKeyBuilder.str(), querySettings);
}

namespace {
// TODO: SERVER-77571 use acquisitions APIs for retrieving the shardVersion.
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
            keyShardingEpoch};
}
}  // namespace

PlanCacheKey make(const CanonicalQuery& query,
                  const CollectionPtr& collection,
                  PlanCacheKeyTag<PlanCacheKey> tag) {
    auto shapeString = canonical_query_encoder::encodeClassic(query);
    return {
        plan_cache_detail::makePlanCacheKeyInfo(std::move(shapeString),
                                                query.getPrimaryMatchExpression(),
                                                collection,
                                                query.getExpCtx()->getQuerySettings()),
    };
}

sbe::PlanCacheKey make(const CanonicalQuery& query,
                       const CollectionPtr& collection,
                       PlanCacheKeyTag<sbe::PlanCacheKey> tag) {
    return plan_cache_key_factory::make(query, MultipleCollectionAccessor(collection));
}
}  // namespace plan_cache_detail

namespace plan_cache_key_factory {

std::tuple<sbe::PlanCacheKeyCollectionState, std::vector<sbe::PlanCacheKeyCollectionState>>
getCollectionState(OperationContext* opCtx, const MultipleCollectionAccessor& collections) {
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
    secondaryCollectionStates.shrink_to_fit();
    return {mainCollectionState, secondaryCollectionStates};
}

sbe::PlanCacheKey make(const CanonicalQuery& query,
                       const MultipleCollectionAccessor& collections,
                       const bool requiresSbeCompatibility) {
    OperationContext* opCtx = query.getOpCtx();
    auto [mainCollectionState, secondaryCollectionStates] = getCollectionState(opCtx, collections);
    auto shapeString = canonical_query_encoder::encodeSBE(query, requiresSbeCompatibility);
    return {plan_cache_detail::makePlanCacheKeyInfo(std::move(shapeString),
                                                    query.getPrimaryMatchExpression(),
                                                    collections.getMainCollection(),
                                                    query.getExpCtx()->getQuerySettings()),
            std::move(mainCollectionState),
            std::move(secondaryCollectionStates)};
}

sbe::PlanCacheKey make(const Pipeline& query, const MultipleCollectionAccessor& collections) {
    OperationContext* opCtx = query.getContext()->getOperationContext();
    auto [mainCollectionState, secondaryCollectionStates] = getCollectionState(opCtx, collections);

    std::vector<boost::intrusive_ptr<DocumentSource>> stages;
    for (auto&& source : query.getSources()) {
        stages.emplace_back(source);
    }

    tassert(8180900, "makePlanCacheKey expects pipeline is non-empty", !stages.empty());

    auto matchStage = dynamic_cast<DocumentSourceMatch*>(stages.front().get());

    auto shapeString = canonical_query_encoder::encodePipeline(query.getContext().get(), stages);
    return {plan_cache_detail::makePlanCacheKeyInfo(std::move(shapeString),
                                                    matchStage->getMatchExpression(),
                                                    collections.getMainCollection(),
                                                    query.getContext()->getQuerySettings()),
            std::move(mainCollectionState),
            std::move(secondaryCollectionStates)};
}
}  // namespace plan_cache_key_factory

}  // namespace mongo
