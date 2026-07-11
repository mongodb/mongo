// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/plan_cache_indexability.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

namespace {

IndexabilityDiscriminator getPartialIndexDiscriminator(const MatchExpression* filterExpr) {
    return [filterExpr](const MatchExpression* queryExpr) {
        return expression::isSubsetOf(queryExpr, filterExpr);
    };
}

IndexabilityDiscriminator getCollatedIndexDiscriminator(const CollatorInterface* collator) {
    return [collator](const MatchExpression* queryExpr) {
        if (const auto* queryExprComparison =
                dynamic_cast<const ComparisonMatchExpressionBase*>(queryExpr)) {
            const bool collatorsMatch =
                CollatorInterface::collatorsMatch(queryExprComparison->getCollator(), collator);
            const bool isCollatableType =
                CollationIndexKey::isCollatableType(queryExprComparison->getData().type());
            return collatorsMatch || !isCollatableType;
        }

        if (queryExpr->matchType() == MatchExpression::MATCH_IN) {
            const auto* queryExprIn = static_cast<const InMatchExpression*>(queryExpr);
            if (CollatorInterface::collatorsMatch(queryExprIn->getCollator(), collator)) {
                return true;
            }
            for (const auto& equality : queryExprIn->getEqualities()) {
                if (CollationIndexKey::isCollatableType(equality.type())) {
                    return false;
                }
            }
            return true;
        }
        // The predicate never compares strings so it is not affected by collation.
        return true;
    };
}

bool nodeIsConservativelySupportedBySparseIndex(const MatchExpression* me) {
    // When an expression is in an $elemMatch, a sparse index may be able to support an equality to
    // null. We don't track whether or not a match expression is in an $elemMatch when generating
    // the plan cache key, so we make the conservative assumption that it is not.
    const bool inElemMatch = false;
    return QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(me, inElemMatch);
}
}  // namespace

void PlanCacheIndexabilityState::processSparseIndex(const std::string& indexName,
                                                    const BSONObj& keyPattern) {
    for (BSONElement elem : keyPattern) {
        _pathDiscriminatorsMap[elem.fieldNameStringData()][indexName].addDiscriminator(
            nodeIsConservativelySupportedBySparseIndex);
    }
}

void PlanCacheIndexabilityState::processPartialIndex(const std::string& indexName,
                                                     const MatchExpression* filterExpr) {
    _globalDiscriminatorMap[indexName].addDiscriminator(getPartialIndexDiscriminator(filterExpr));
}

void PlanCacheIndexabilityState::processWildcardIndex(const CoreIndexInfo& cii) {
    tassert(11177604,
            fmt::format("Expected wildcard index, but found index with key pattern {}",
                        cii.keyPattern.toString()),
            cii.type == IndexType::INDEX_WILDCARD);

    _wildcardIndexDiscriminators.emplace_back(
        cii.indexPathProjection->exec(), cii.identifier.catalogName, cii.collator);
}

void PlanCacheIndexabilityState::processIndexCollation(const std::string& indexName,
                                                       const BSONObj& keyPattern,
                                                       const CollatorInterface* collator) {
    for (BSONElement elem : keyPattern) {
        _pathDiscriminatorsMap[elem.fieldNameStringData()][indexName].addDiscriminator(
            getCollatedIndexDiscriminator(collator));
    }
}

namespace {
const IndexToDiscriminatorMap emptyDiscriminators{};
}  // namespace

const IndexToDiscriminatorMap& PlanCacheIndexabilityState::getPathDiscriminators(
    std::string_view path) const {
    PathDiscriminatorsMap::const_iterator it = _pathDiscriminatorsMap.find(path);
    if (it == _pathDiscriminatorsMap.end()) {
        return emptyDiscriminators;
    }
    return it->second;
}

IndexToDiscriminatorMap PlanCacheIndexabilityState::buildWildcardDiscriminators(
    std::string_view path) const {

    IndexToDiscriminatorMap ret;
    for (auto&& wildcardDiscriminator : _wildcardIndexDiscriminators) {
        if (projection_executor_utils::applyProjectionToOneField(
                wildcardDiscriminator.projectionExec, path)) {
            CompositeIndexabilityDiscriminator& cid = ret[wildcardDiscriminator.catalogName];

            // We can use these 'shallow' functions because the code building the plan cache key
            // will descend the match expression for us, and check the discriminator's return value
            // at each node.
            cid.addDiscriminator(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex);
            cid.addDiscriminator(nodeIsConservativelySupportedBySparseIndex);
            cid.addDiscriminator(getCollatedIndexDiscriminator(wildcardDiscriminator.collator));
        }
    }
    return ret;
}

void PlanCacheIndexabilityState::updateDiscriminators(
    const std::vector<CoreIndexInfo>& indexCores) {
    _pathDiscriminatorsMap = PathDiscriminatorsMap();
    _globalDiscriminatorMap = IndexToDiscriminatorMap();
    _wildcardIndexDiscriminators.clear();

    for (const auto& idx : indexCores) {
        // If necessary, add discriminators for the paths mentioned in the partial filter
        // expression. Unlike most of the discriminator logic, this is shared for wildcard and
        // non-wildcard indexes.
        if (idx.filterExpr) {
            processPartialIndex(idx.identifier.catalogName, idx.filterExpr);
        }

        if (idx.type == IndexType::INDEX_WILDCARD) {
            // The set of paths for which we should add disciminators for wildcard indexes (outside
            // of those paths mentioned in the partial filter expression) is not known a priori.
            // Instead, we just record some information about the wildcard index so that the
            // discriminators can be constructed on demand at query runtime.
            processWildcardIndex(idx);
        }

        if (idx.sparse) {
            processSparseIndex(idx.identifier.catalogName, idx.keyPattern);
        }

        processIndexCollation(idx.identifier.catalogName, idx.keyPattern, idx.collator);
    }
}

}  // namespace mongo
