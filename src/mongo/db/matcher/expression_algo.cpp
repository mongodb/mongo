// expression_algo.cpp

/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/base/checked_cast.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {

using std::unique_ptr;

namespace {

bool supportsEquality(const ComparisonMatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::LTE:
        case MatchExpression::EQ:
        case MatchExpression::GTE:
            return true;
        default:
            return false;
    }
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const ComparisonMatchExpression* lhs, const ComparisonMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    const BSONElement lhsData = lhs->getData();
    const BSONElement rhsData = rhs->getData();

    if (lhsData.canonicalType() != rhsData.canonicalType()) {
        return false;
    }

    // Special case the handling for NaN values: NaN compares equal only to itself.
    if (std::isnan(lhsData.numberDouble()) || std::isnan(rhsData.numberDouble())) {
        if (supportsEquality(lhs) && supportsEquality(rhs)) {
            return std::isnan(lhsData.numberDouble()) && std::isnan(rhsData.numberDouble());
        }
        return false;
    }

    if (!CollatorInterface::collatorsMatch(lhs->getCollator(), rhs->getCollator()) &&
        CollationIndexKey::isCollatableType(lhsData.type())) {
        return false;
    }

    // Either collator may be used by compareElementValues() here, since either the collators are
    // the same or lhsData does not contain string comparison.
    int cmp = compareElementValues(lhsData, rhsData, rhs->getCollator());

    // Check whether the two expressions are equivalent.
    if (lhs->matchType() == rhs->matchType() && cmp == 0) {
        return true;
    }

    switch (rhs->matchType()) {
        case MatchExpression::LT:
        case MatchExpression::LTE:
            switch (lhs->matchType()) {
                case MatchExpression::LT:
                case MatchExpression::LTE:
                case MatchExpression::EQ:
                    if (rhs->matchType() == MatchExpression::LTE) {
                        return cmp <= 0;
                    }
                    return cmp < 0;
                default:
                    return false;
            }
        case MatchExpression::GT:
        case MatchExpression::GTE:
            switch (lhs->matchType()) {
                case MatchExpression::GT:
                case MatchExpression::GTE:
                case MatchExpression::EQ:
                    if (rhs->matchType() == MatchExpression::GTE) {
                        return cmp >= 0;
                    }
                    return cmp > 0;
                default:
                    return false;
            }
        default:
            return false;
    }
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const MatchExpression* lhs, const ComparisonMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    if (ComparisonMatchExpression::isComparisonMatchExpression(lhs)) {
        return _isSubsetOf(static_cast<const ComparisonMatchExpression*>(lhs), rhs);
    }

    if (lhs->matchType() == MatchExpression::MATCH_IN) {
        const InMatchExpression* ime = static_cast<const InMatchExpression*>(lhs);
        if (!ime->getRegexes().empty()) {
            return false;
        }
        for (BSONElement elem : ime->getEqualities()) {
            // Each element in the $in-array represents an equality predicate.
            EqualityMatchExpression equality;
            equality.init(lhs->path(), elem);
            equality.setCollator(ime->getCollator());
            if (!_isSubsetOf(&equality, rhs)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const MatchExpression* lhs, const ExistsMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field. Defer checking the path for $not expressions until the
    // subexpression is examined.
    if (lhs->matchType() != MatchExpression::NOT && lhs->path() != rhs->path()) {
        return false;
    }

    if (ComparisonMatchExpression::isComparisonMatchExpression(lhs)) {
        const ComparisonMatchExpression* cme = static_cast<const ComparisonMatchExpression*>(lhs);
        // CompareMatchExpression::init() prohibits creating a match expression with EOO or
        // Undefined types, so only need to ensure that the value is not of type jstNULL.
        return cme->getData().type() != jstNULL;
    }

    switch (lhs->matchType()) {
        case MatchExpression::ELEM_MATCH_VALUE:
        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::EXISTS:
        case MatchExpression::GEO:
        case MatchExpression::MOD:
        case MatchExpression::REGEX:
        case MatchExpression::SIZE:
        case MatchExpression::TYPE_OPERATOR:
            return true;
        case MatchExpression::MATCH_IN: {
            const InMatchExpression* ime = static_cast<const InMatchExpression*>(lhs);
            return !ime->hasNull();
        }
        case MatchExpression::NOT:
            // An expression can only match a subset of the documents matched by another if they are
            // comparing the same field.
            if (lhs->getChild(0)->path() != rhs->path()) {
                return false;
            }

            switch (lhs->getChild(0)->matchType()) {
                case MatchExpression::EQ: {
                    const ComparisonMatchExpression* cme =
                        static_cast<const ComparisonMatchExpression*>(lhs->getChild(0));
                    return cme->getData().type() == jstNULL;
                }
                case MatchExpression::MATCH_IN: {
                    const InMatchExpression* ime =
                        static_cast<const InMatchExpression*>(lhs->getChild(0));
                    return ime->hasNull();
                }
                default:
                    return false;
            }
        default:
            return false;
    }
}

/**
 * Returns whether the leaf is a $elemMatch expression.
 */
bool isElemMatch(const MatchExpression& expr) {
    return expr.matchType() == MatchExpression::ELEM_MATCH_OBJECT ||
        expr.matchType() == MatchExpression::ELEM_MATCH_VALUE;
}


/**
 * Returns whether the leaf at 'path' is independent of 'fields'.
 */
bool isLeafIndependentOf(const StringData& path, const std::set<std::string>& fields) {
    // For each field in 'fields', we need to check if that field is a prefix of 'path' or if 'path'
    // is a prefix of that field. For example, the expression {a.b: {c: 1}} is not independent of
    // 'a.b.c', and and the expression {a.b.c.d: 1} is not independent of 'a.b.c'.
    for (StringData field : fields) {
        if (path == field || expression::isPathPrefixOf(path, field) ||
            expression::isPathPrefixOf(field, path)) {
            return false;
        }
    }
    return true;
}

/**
 * Creates a MatchExpression that is equivalent to {$and: [children[0], children[1]...]}.
 */
unique_ptr<MatchExpression> createAndOfNodes(std::vector<unique_ptr<MatchExpression>>* children) {
    if (children->empty()) {
        return nullptr;
    }

    if (children->size() == 1) {
        return std::move(children->at(0));
    }

    unique_ptr<AndMatchExpression> splitAnd = stdx::make_unique<AndMatchExpression>();
    for (auto&& expr : *children) {
        splitAnd->add(expr.release());
    }

    return std::move(splitAnd);
}

/**
 * Creates a MatchExpression that is equivalent to {$nor: [children[0], children[1]...]}.
 */
unique_ptr<MatchExpression> createNorOfNodes(std::vector<unique_ptr<MatchExpression>>* children) {
    if (children->empty()) {
        return nullptr;
    }

    unique_ptr<NorMatchExpression> splitNor = stdx::make_unique<NorMatchExpression>();
    for (auto&& expr : *children) {
        splitNor->add(expr.release());
    }

    return std::move(splitNor);
}

}  // namespace

namespace expression {

bool isSubsetOf(const MatchExpression* lhs, const MatchExpression* rhs) {
    invariant(lhs);
    invariant(rhs);

    if (lhs->equivalent(rhs)) {
        return true;
    }

    if (rhs->matchType() == MatchExpression::AND) {
        // 'lhs' must match a subset of the documents matched by each clause of 'rhs'.
        for (size_t i = 0; i < rhs->numChildren(); i++) {
            if (!isSubsetOf(lhs, rhs->getChild(i))) {
                return false;
            }
        }
        return true;
    }

    if (lhs->matchType() == MatchExpression::AND) {
        // At least one clause of 'lhs' must match a subset of the documents matched by 'rhs'.
        for (size_t i = 0; i < lhs->numChildren(); i++) {
            if (isSubsetOf(lhs->getChild(i), rhs)) {
                return true;
            }
        }
        return false;
    }

    if (lhs->matchType() == MatchExpression::OR) {
        // Every clause of 'lhs' must match a subset of the documents matched by 'rhs'.
        for (size_t i = 0; i < lhs->numChildren(); i++) {
            if (!isSubsetOf(lhs->getChild(i), rhs)) {
                return false;
            }
        }
        return true;
    }

    if (ComparisonMatchExpression::isComparisonMatchExpression(rhs)) {
        return _isSubsetOf(lhs, static_cast<const ComparisonMatchExpression*>(rhs));
    }

    if (rhs->matchType() == MatchExpression::EXISTS) {
        return _isSubsetOf(lhs, static_cast<const ExistsMatchExpression*>(rhs));
    }

    return false;
}

bool isIndependentOf(const MatchExpression& expr, const std::set<std::string>& pathSet) {
    if (expr.isLogical()) {
        // Any logical expression is independent of 'pathSet' if all its children are independent of
        // 'pathSet'.
        for (size_t i = 0; i < expr.numChildren(); i++) {
            if (!isIndependentOf(*expr.getChild(i), pathSet)) {
                return false;
            }
        }
        return true;
    }

    // At this point, we know 'expr' is a leaf. If it is an elemMatch, we do not attempt to
    // determine if it is independent or not, and instead just return false.
    return !isElemMatch(expr) && isLeafIndependentOf(expr.path(), pathSet);
}

std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitMatchExpressionBy(
    unique_ptr<MatchExpression> expr, const std::set<std::string>& fields) {
    if (isIndependentOf(*expr, fields)) {
        // 'expr' does not depend upon 'fields', so it can be completely moved.
        return {std::move(expr), nullptr};
    }
    if (!expr->isLogical()) {
        // 'expr' is a leaf, and was not independent of 'fields'.
        return {nullptr, std::move(expr)};
    }

    std::vector<unique_ptr<MatchExpression>> reliant;
    std::vector<unique_ptr<MatchExpression>> separate;

    switch (expr->matchType()) {
        case MatchExpression::AND: {
            auto andExpr = checked_cast<AndMatchExpression*>(expr.get());
            for (size_t i = 0; i < andExpr->numChildren(); i++) {
                auto children = splitMatchExpressionBy(andExpr->releaseChild(i), fields);

                invariant(children.first || children.second);

                if (children.first) {
                    separate.push_back(std::move(children.first));
                }
                if (children.second) {
                    reliant.push_back(std::move(children.second));
                }
            }
            return {createAndOfNodes(&separate), createAndOfNodes(&reliant)};
        }
        case MatchExpression::NOR: {
            // We can split a $nor because !(x | y) is logically equivalent to !x & !y.

            // However, we cannot split each child individually; instead, we must look for a wholly
            // independent child to split off by itself. As an example of why, with 'b' in
            // 'fields': $nor: [{$and: [{a: 1}, {b: 1}]}]} will match if a is not 1, or if b is not
            // 1. However, if we split this into: {$nor: [{$and: [{a: 1}]}]}, and
            // {$nor: [{$and: [{b: 1}]}]}, a document will only pass both stages if neither a nor b
            // is equal to 1.
            auto norExpr = checked_cast<NorMatchExpression*>(expr.get());
            for (size_t i = 0; i < norExpr->numChildren(); i++) {
                auto child = norExpr->releaseChild(i);
                if (isIndependentOf(*child, fields)) {
                    separate.push_back(std::move(child));
                } else {
                    reliant.push_back(std::move(child));
                }
            }
            return {createNorOfNodes(&separate), createNorOfNodes(&reliant)};
        }
        case MatchExpression::OR:
        case MatchExpression::NOT: {
            // If we aren't independent, we can't safely split.
            return {nullptr, std::move(expr)};
        }
        default: { MONGO_UNREACHABLE; }
    }
}

void mapOver(MatchExpression* expr, NodeTraversalFunc func, std::string path) {
    if (!expr->path().empty()) {
        if (!path.empty()) {
            path += ".";
        }

        path += expr->path().toString();
    }

    for (size_t i = 0; i < expr->numChildren(); i++) {
        mapOver(expr->getChild(i), func, path);
    }

    func(expr, path);
}

bool isPathPrefixOf(StringData first, StringData second) {
    if (first.size() >= second.size()) {
        return false;
    }

    return second.startsWith(first) && second[first.size()] == '.';
}

}  // namespace expression
}  // namespace mongo
