/**
 *    Copyright (C) 2015 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_cache_indexability.h"

#include "mongo/base/init.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/index/all_paths_key_generator.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_internal_expr_eq.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/stdx/memory.h"
#include <memory>

namespace mongo {

namespace {

bool canUseAllPathsIndex(BSONElement elt, MatchExpression::MatchType matchType) {
    if (elt.type() == BSONType::Object) {
        return false;
    }

    if (elt.type() == BSONType::Array) {
        // We only support equality to empty array.
        return elt.embeddedObject().isEmpty() && matchType == MatchExpression::EQ;
    }

    return true;
}

bool supportedByAllPathsIndex(const MatchExpression* queryExpr) {
    if (ComparisonMatchExpression::isComparisonMatchExpression(queryExpr)) {
        const ComparisonMatchExpression* cmpExpr =
            static_cast<const ComparisonMatchExpression*>(queryExpr);

        return canUseAllPathsIndex(cmpExpr->getData(), cmpExpr->matchType());
    } else if (queryExpr->matchType() == MatchExpression::MATCH_IN) {
        const auto* queryExprIn = static_cast<const InMatchExpression*>(queryExpr);

        return std::all_of(
            queryExprIn->getEqualities().begin(),
            queryExprIn->getEqualities().end(),
            [](const BSONElement& elt) { return canUseAllPathsIndex(elt, MatchExpression::EQ); });
    }

    return true;
};

bool supportedBySparseIndex(const MatchExpression* queryExpr) {
    if (queryExpr->matchType() == MatchExpression::EQ) {
        const auto* queryExprEquality = static_cast<const EqualityMatchExpression*>(queryExpr);
        return !queryExprEquality->getData().isNull();
    } else if (queryExpr->matchType() == MatchExpression::MATCH_IN) {
        const auto* queryExprIn = static_cast<const InMatchExpression*>(queryExpr);
        return !queryExprIn->hasNull();
    } else {
        return true;
    }
};

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
}

void PlanCacheIndexabilityState::processSparseIndex(const std::string& indexName,
                                                    const BSONObj& keyPattern) {
    for (BSONElement elem : keyPattern) {
        _pathDiscriminatorsMap[elem.fieldNameStringData()][indexName].addDiscriminator(
            supportedBySparseIndex);
    }
}

void PlanCacheIndexabilityState::processPartialIndex(const std::string& indexName,
                                                     const MatchExpression* filterExpr) {
    invariant(filterExpr);
    for (size_t i = 0; i < filterExpr->numChildren(); ++i) {
        processPartialIndex(indexName, filterExpr->getChild(i));
    }
    if (filterExpr->getCategory() != MatchExpression::MatchCategory::kLogical) {
        _pathDiscriminatorsMap[filterExpr->path()][indexName].addDiscriminator(
            getPartialIndexDiscriminator(filterExpr));
    }
}

void PlanCacheIndexabilityState::processAllPathsIndex(const IndexEntry& ie) {
    invariant(ie.type == IndexType::INDEX_ALLPATHS);

    _allPathsIndexDiscriminators.emplace_back(
        AllPathsKeyGenerator::createProjectionExec(ie.keyPattern,
                                                   ie.infoObj.getObjectField("starPathsTempName")),
        ie.identifier.catalogName,
        ie.filterExpr,
        ie.collator);
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

const IndexToDiscriminatorMap& PlanCacheIndexabilityState::getDiscriminators(
    StringData path) const {
    PathDiscriminatorsMap::const_iterator it = _pathDiscriminatorsMap.find(path);
    if (it == _pathDiscriminatorsMap.end()) {
        return emptyDiscriminators;
    }
    return it->second;
}

IndexToDiscriminatorMap PlanCacheIndexabilityState::buildAllPathsDiscriminators(
    StringData path) const {

    IndexToDiscriminatorMap ret;
    for (auto&& allPathsDiscriminator : _allPathsIndexDiscriminators) {
        if (allPathsDiscriminator.projectionExec->applyProjectionToOneField(path)) {
            CompositeIndexabilityDiscriminator& cid = ret[allPathsDiscriminator.catalogName];

            cid.addDiscriminator(supportedByAllPathsIndex);
            cid.addDiscriminator(supportedBySparseIndex);
            cid.addDiscriminator(getCollatedIndexDiscriminator(allPathsDiscriminator.collator));
            if (allPathsDiscriminator.filterExpr) {
                cid.addDiscriminator(
                    getPartialIndexDiscriminator(allPathsDiscriminator.filterExpr));
            }
        }
    }
    return ret;
}

void PlanCacheIndexabilityState::updateDiscriminators(const std::vector<IndexEntry>& indexEntries) {
    _pathDiscriminatorsMap = PathDiscriminatorsMap();

    for (const IndexEntry& idx : indexEntries) {
        if (idx.type == IndexType::INDEX_ALLPATHS) {
            processAllPathsIndex(idx);
            continue;
        }

        if (idx.sparse) {
            processSparseIndex(idx.identifier.catalogName, idx.keyPattern);
        }
        if (idx.filterExpr) {
            processPartialIndex(idx.identifier.catalogName, idx.filterExpr);
        }

        processIndexCollation(idx.identifier.catalogName, idx.keyPattern, idx.collator);
    }
}

}  // namespace mongo
