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
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/stdx/memory.h"
#include <memory>

namespace mongo {

void PlanCacheIndexabilityState::processSparseIndex(const std::string& indexName,
                                                    const BSONObj& keyPattern) {
    for (BSONElement elem : keyPattern) {
        _pathDiscriminatorsMap[elem.fieldNameStringData()][indexName].addDiscriminator(
            [](const MatchExpression* queryExpr) {
                if (queryExpr->matchType() == MatchExpression::EQ) {
                    const auto* queryExprEquality =
                        static_cast<const EqualityMatchExpression*>(queryExpr);
                    return !queryExprEquality->getData().isNull();
                } else if (queryExpr->matchType() == MatchExpression::MATCH_IN) {
                    const auto* queryExprIn = static_cast<const InMatchExpression*>(queryExpr);
                    return !queryExprIn->hasNull();
                } else {
                    return true;
                }
            });
    }
}

void PlanCacheIndexabilityState::processPartialIndex(const std::string& indexName,
                                                     const MatchExpression* filterExpr) {
    invariant(filterExpr);
    for (size_t i = 0; i < filterExpr->numChildren(); ++i) {
        processPartialIndex(indexName, filterExpr->getChild(i));
    }
    if (!filterExpr->isLogical()) {
        _pathDiscriminatorsMap[filterExpr->path()][indexName].addDiscriminator(
            [filterExpr](const MatchExpression* queryExpr) {
                return expression::isSubsetOf(queryExpr, filterExpr);
            });
    }
}

void PlanCacheIndexabilityState::processIndexCollation(const std::string& indexName,
                                                       const BSONObj& keyPattern,
                                                       const CollatorInterface* collator) {
    for (BSONElement elem : keyPattern) {
        _pathDiscriminatorsMap[elem.fieldNameStringData()][indexName].addDiscriminator([collator](
            const MatchExpression* queryExpr) {
            if (ComparisonMatchExpression::isComparisonMatchExpression(queryExpr)) {
                const auto* queryExprComparison =
                    static_cast<const ComparisonMatchExpression*>(queryExpr);
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
        });
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

void PlanCacheIndexabilityState::updateDiscriminators(const std::vector<IndexEntry>& indexEntries) {
    _pathDiscriminatorsMap = PathDiscriminatorsMap();

    for (const IndexEntry& idx : indexEntries) {
        if (idx.sparse) {
            processSparseIndex(idx.name, idx.keyPattern);
        }
        if (idx.filterExpr) {
            processPartialIndex(idx.name, idx.filterExpr);
        }
        processIndexCollation(idx.name, idx.keyPattern, idx.collator);
    }
}

}  // namespace mongo
