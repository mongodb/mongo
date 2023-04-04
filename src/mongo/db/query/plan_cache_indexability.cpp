/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_cache_indexability.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/planner_ixselect.h"
#include <memory>

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
    invariant(cii.type == IndexType::INDEX_WILDCARD);

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
    StringData path) const {
    PathDiscriminatorsMap::const_iterator it = _pathDiscriminatorsMap.find(path);
    if (it == _pathDiscriminatorsMap.end()) {
        return emptyDiscriminators;
    }
    return it->second;
}

IndexToDiscriminatorMap PlanCacheIndexabilityState::buildWildcardDiscriminators(
    StringData path) const {

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

        // (Ignore FCV check): This is intentional because we want clusters which have wildcard
        // indexes still be able to use the feature even if the FCV is downgraded.
        if (feature_flags::gFeatureFlagCompoundWildcardIndexes.isEnabledAndIgnoreFCVUnsafe() ||
            idx.type != IndexType::INDEX_WILDCARD) {
            // If the index is wildcard compound or not wildcard, add discriminators for
            // fields mentioned in the key pattern.
            if (idx.sparse) {
                processSparseIndex(idx.identifier.catalogName, idx.keyPattern);
            }

            processIndexCollation(idx.identifier.catalogName, idx.keyPattern, idx.collator);
        }
    }
}

}  // namespace mongo
