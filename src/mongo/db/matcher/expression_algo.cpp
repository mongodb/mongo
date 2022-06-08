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

#include "mongo/base/checked_cast.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
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

    // Either collator may be used by compareElements() here, since either the collators are
    // the same or lhsData does not contain string comparison.
    int cmp = BSONElement::compareElements(
        lhsData, rhsData, BSONElement::ComparisonRules::kConsiderFieldName, rhs->getCollator());

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

bool _isSubsetOfInternalExpr(const ComparisonMatchExpressionBase* lhs,
                             const ComparisonMatchExpressionBase* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    const BSONElement lhsData = lhs->getData();
    const BSONElement rhsData = rhs->getData();

    if (!CollatorInterface::collatorsMatch(lhs->getCollator(), rhs->getCollator()) &&
        CollationIndexKey::isCollatableType(lhsData.type())) {
        return false;
    }

    int cmp = lhsData.woCompare(
        rhsData, BSONElement::ComparisonRules::kConsiderFieldName, rhs->getCollator());

    // Check whether the two expressions are equivalent.
    if (lhs->matchType() == rhs->matchType() && cmp == 0) {
        return true;
    }

    switch (rhs->matchType()) {
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::INTERNAL_EXPR_LTE:
            switch (lhs->matchType()) {
                case MatchExpression::INTERNAL_EXPR_LT:
                case MatchExpression::INTERNAL_EXPR_LTE:
                case MatchExpression::INTERNAL_EXPR_EQ:
                    //
                    if (rhs->matchType() == MatchExpression::LTE) {
                        return cmp <= 0;
                    }
                    return cmp < 0;
                default:
                    return false;
            }
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::INTERNAL_EXPR_GTE:
            switch (lhs->matchType()) {
                case MatchExpression::INTERNAL_EXPR_GT:
                case MatchExpression::INTERNAL_EXPR_GTE:
                case MatchExpression::INTERNAL_EXPR_EQ:
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
 *
 * This overload handles the $_internalExpr family of comparisons.
 */
bool _isSubsetOfInternalExpr(const MatchExpression* lhs, const ComparisonMatchExpressionBase* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    if (ComparisonMatchExpressionBase::isInternalExprComparison(lhs->matchType())) {
        return _isSubsetOfInternalExpr(static_cast<const ComparisonMatchExpressionBase*>(lhs), rhs);
    }

    return false;
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 *
 * This overload handles comparisons such as $lt, $eq, $gte, but not $_internalExprLt, etc.
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
            EqualityMatchExpression equality(lhs->path(), elem);
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
bool _isSubsetOf(const MatchExpression* lhs, const InMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    if (!rhs->getRegexes().empty()) {
        return false;
    }

    for (BSONElement elem : rhs->getEqualities()) {
        // Each element in the $in-array represents an equality predicate.
        EqualityMatchExpression equality(rhs->path(), elem);
        equality.setCollator(rhs->getCollator());
        if (_isSubsetOf(lhs, &equality)) {
            return true;
        }
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
        // The CompareMatchExpression constructor prohibits creating a match expression with EOO or
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
 * Creates a MatchExpression that is equivalent to {$and: [children[0], children[1]...]}.
 */
unique_ptr<MatchExpression> createAndOfNodes(std::vector<unique_ptr<MatchExpression>>* children) {
    if (children->empty()) {
        return nullptr;
    }

    if (children->size() == 1) {
        return std::move(children->at(0));
    }

    unique_ptr<AndMatchExpression> splitAnd = std::make_unique<AndMatchExpression>();
    for (auto&& expr : *children)
        splitAnd->add(std::move(expr));

    return splitAnd;
}

/**
 * Creates a MatchExpression that is equivalent to {$nor: [children[0], children[1]...]}.
 */
unique_ptr<MatchExpression> createNorOfNodes(std::vector<unique_ptr<MatchExpression>>* children) {
    if (children->empty()) {
        return nullptr;
    }

    unique_ptr<NorMatchExpression> splitNor = std::make_unique<NorMatchExpression>();
    for (auto&& expr : *children)
        splitNor->add(std::move(expr));

    return splitNor;
}

/**
 * Attempt to split 'expr' into two MatchExpressions according to 'shouldSplitOut', which describes
 * the conditions under which its argument can be split from 'expr'. Returns two pointers, where
 * each new MatchExpression contains a portion of 'expr'. The first contains the parts of 'expr'
 * which satisfy 'shouldSplitOut', and the second are the remaining parts of 'expr'.
 */
std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitMatchExpressionByFunction(
    unique_ptr<MatchExpression> expr,
    const OrderedPathSet& fields,
    expression::ShouldSplitExprFunc shouldSplitOut) {
    if (shouldSplitOut(*expr, fields)) {
        // 'expr' satisfies our split condition and can be completely split out.
        return {std::move(expr), nullptr};
    }

    if (expr->getCategory() != MatchExpression::MatchCategory::kLogical) {
        // 'expr' is a leaf and cannot be split out.
        return {nullptr, std::move(expr)};
    }

    std::vector<unique_ptr<MatchExpression>> splitOut;
    std::vector<unique_ptr<MatchExpression>> remaining;

    switch (expr->matchType()) {
        case MatchExpression::AND: {
            auto andExpr = checked_cast<AndMatchExpression*>(expr.get());
            for (size_t i = 0; i < andExpr->numChildren(); i++) {
                auto children = splitMatchExpressionByFunction(
                    andExpr->releaseChild(i), fields, shouldSplitOut);

                invariant(children.first || children.second);

                if (children.first) {
                    splitOut.push_back(std::move(children.first));
                }
                if (children.second) {
                    remaining.push_back(std::move(children.second));
                }
            }
            return {createAndOfNodes(&splitOut), createAndOfNodes(&remaining)};
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
                if (shouldSplitOut(*child, fields)) {
                    splitOut.push_back(std::move(child));
                } else {
                    remaining.push_back(std::move(child));
                }
            }
            return {createNorOfNodes(&splitOut), createNorOfNodes(&remaining)};
        }
        case MatchExpression::OR:
        case MatchExpression::INTERNAL_SCHEMA_XOR:
        case MatchExpression::NOT: {
            // We haven't satisfied the split condition, so 'expr' belongs in the remaining match.
            return {nullptr, std::move(expr)};
        }
        default: { MONGO_UNREACHABLE; }
    }
}

bool pathDependenciesAreExact(StringData key, const MatchExpression* expr) {
    DepsTracker columnDeps;
    expr->addDependencies(&columnDeps);
    return !columnDeps.needWholeDocument && columnDeps.fields == OrderedPathSet{key.toString()};
}

void addExpr(StringData path,
             std::unique_ptr<MatchExpression> me,
             StringMap<std::unique_ptr<MatchExpression>>& out) {
    // In order for this to be correct, the dependencies of the filter by column must be exactly
    // this column.
    dassert(pathDependenciesAreExact(path, me.get()));
    auto& entryForPath = out[path];
    if (!entryForPath) {
        // First predicate for this path, just put it in directly.
        entryForPath = std::move(me);
    } else {
        // We have at least one predicate for this path already. Put all the predicates for the path
        // into a giant $and clause. Note this might have to change once we start supporting $or
        // predicates.
        if (entryForPath->matchType() != MatchExpression::AND) {
            // This is the second predicate, we need to make the $and and put in both predicates:
            // {$and: [<existing>, 'me']}.
            auto andME = std::make_unique<AndMatchExpression>();
            andME->add(std::move(entryForPath));
            entryForPath = std::move(andME);
        }
        auto andME = checked_cast<AndMatchExpression*>(entryForPath.get());
        andME->add(std::move(me));
    }
}

std::unique_ptr<MatchExpression> tryAddExpr(StringData path,
                                            const MatchExpression* me,
                                            StringMap<std::unique_ptr<MatchExpression>>& out) {
    if (FieldRef(path).hasNumericPathComponents())
        return me->shallowClone();

    addExpr(path, me->shallowClone(), out);
    return nullptr;
}

/**
 * Helper for the main public API. Returns only the residual predicate and adds any columnar
 * predicates into 'out'.
 */
std::unique_ptr<MatchExpression> splitMatchExpressionForColumns(
    const MatchExpression* me, StringMap<std::unique_ptr<MatchExpression>>& out) {
    auto canCompareWith = [](const BSONElement& elem, bool isEQ) {
        // Here we check whether the comparison can work with the given value. Objects and arrays
        // are generally not permitted. Objects can't work because the paths will be split apart in
        // the columnar index. We could do arrays of scalars since we would have all that
        // information in the index, but it proved complex to integrate due to the interface with
        // the matcher. It expects to get a BSONElement for the whole Array but we'd like to avoid
        // materializing that.
        //
        // One exception to the above: We can support EQ with empty objects and empty arrays since
        // those are more obviously correct. Maybe could also support LT and LTE, but those don't
        // seem as important so are left for future work.
        if (elem.type() == BSONType::Array || elem.type() == BSONType::Object) {
            return isEQ && elem.Obj().isEmpty();
        }

        // We support all other types, except null, since it is equivalent to x==null || !exists(x).
        return !elem.isNull();
    };
    switch (me->matchType()) {
        // These are always safe since they will never match documents missing their field, or where
        // the element is an object or array.
        case MatchExpression::REGEX:
        case MatchExpression::MOD:
        case MatchExpression::BITS_ALL_SET:
        case MatchExpression::BITS_ALL_CLEAR:
        case MatchExpression::BITS_ANY_SET:
        case MatchExpression::BITS_ANY_CLEAR:
        case MatchExpression::EXISTS: {
            // Note: {$exists: false} is represented as {$not: {$exists: true}}.
            auto sub = checked_cast<const PathMatchExpression*>(me);
            return tryAddExpr(sub->path(), me, out);
        }

        case MatchExpression::LT:
        case MatchExpression::GT:
        case MatchExpression::EQ:
        case MatchExpression::LTE:
        case MatchExpression::GTE: {
            auto sub = checked_cast<const ComparisonMatchExpressionBase*>(me);
            if (!canCompareWith(sub->getData(), me->matchType() == MatchExpression::EQ))
                return me->shallowClone();
            return tryAddExpr(sub->path(), me, out);
        }


        case MatchExpression::MATCH_IN: {
            auto sub = checked_cast<const InMatchExpression*>(me);
            // Note that $in treats regexes specially and stores them separately than the rest of
            // the 'equalities'. We actually don't need to look at them here since any regex should
            // be OK. A regex could only match a string, symbol, or other regex, any of which would
            // be present in the columnar storage.
            for (auto&& elem : sub->getEqualities()) {
                if (!canCompareWith(elem, true))
                    return me->shallowClone();
            }
            return tryAddExpr(sub->path(), me, out);
        }

        case MatchExpression::TYPE_OPERATOR: {
            auto sub = checked_cast<const TypeMatchExpression*>(me);
            tassert(6430600,
                    "Not expecting to find EOO in a $type expression",
                    !sub->typeSet().hasType(BSONType::EOO));
            if (sub->typeSet().hasType(BSONType::Object) || sub->typeSet().hasType(BSONType::Array))
                return me->shallowClone();
            return tryAddExpr(sub->path(), me, out);
        }

        case MatchExpression::AND: {
            auto originalAnd = checked_cast<const AndMatchExpression*>(me);
            std::vector<std::unique_ptr<MatchExpression>> newChildren;
            for (size_t i = 0, end = originalAnd->numChildren(); i != end; ++i) {
                if (auto residual = splitMatchExpressionForColumns(originalAnd->getChild(i), out)) {
                    newChildren.emplace_back(std::move(residual));
                }
            }
            if (newChildren.empty()) {
                return nullptr;
            }
            return newChildren.size() == 1
                ? std::move(newChildren[0])
                : std::make_unique<AndMatchExpression>(std::move(newChildren));
        }


        case MatchExpression::NOT: {
            // {$ne: null} pattern is known to be important in cases like those in SERVER-27646 and
            // SERVER-36465.
            auto notExpr = checked_cast<const NotMatchExpression*>(me);
            auto withinNot = notExpr->getChild(0);

            // Oddly, we parse {$ne: null} to a NOT -> EQ, but we parse {$not: {$eq: null}} into a
            // more complex NOT -> AND -> EQ. Let's support both.
            auto tryAddNENull = [&](const MatchExpression* negatedPred) {
                if (negatedPred->matchType() != MatchExpression::EQ) {
                    return false;
                }
                auto eqPred = checked_cast<const EqualityMatchExpression*>(negatedPred);
                if (eqPred->getData().isNull()) {
                    return tryAddExpr(eqPred->path(), me, out) == nullptr;
                }
                return false;
            };
            if (tryAddNENull(withinNot)) {
                // {$ne: null}. We had equality just under NOT.
                return nullptr;
            } else if (withinNot->matchType() == MatchExpression::AND &&
                       withinNot->numChildren() == 1 && tryAddNENull(withinNot->getChild(0))) {
                // {$not: {$eq: null}}: NOT -> AND -> EQ.
                return nullptr;
            }
            // May be other cases, but left as future work.
            return me->shallowClone();
        }

        // We don't currently handle any of these cases, but some may be possible in the future.
        case MatchExpression::ALWAYS_FALSE:
        case MatchExpression::ALWAYS_TRUE:
        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::ELEM_MATCH_VALUE:  // This one should be feasible. May be valuable.
        case MatchExpression::ENCRYPTED_BETWEEN:
        case MatchExpression::EXPRESSION:
        case MatchExpression::GEO:
        case MatchExpression::GEO_NEAR:
        case MatchExpression::INTERNAL_2D_POINT_IN_ANNULUS:
        case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
        case MatchExpression::INTERNAL_EXPR_EQ:  // This one could be valuable for $lookup
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::INTERNAL_EXPR_GTE:
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::INTERNAL_EXPR_LTE:
        case MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE:
        case MatchExpression::INTERNAL_SCHEMA_COND:
        case MatchExpression::INTERNAL_SCHEMA_EQ:
        case MatchExpression::INTERNAL_SCHEMA_FMOD:
        case MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
        case MatchExpression::INTERNAL_SCHEMA_MAX_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_MAX_LENGTH:
        case MatchExpression::INTERNAL_SCHEMA_MAX_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_MIN_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_MIN_LENGTH:
        case MatchExpression::INTERNAL_SCHEMA_MIN_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH:
        case MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ:
        case MatchExpression::INTERNAL_SCHEMA_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_XOR:
        case MatchExpression::NOR:
        case MatchExpression::OR:
        case MatchExpression::SIZE:
        case MatchExpression::TEXT:
        case MatchExpression::WHERE:
            return me->shallowClone();
    }
    MONGO_UNREACHABLE;
}

}  // namespace

namespace expression {

bool hasExistencePredicateOnPath(const MatchExpression& expr, StringData path) {
    if (expr.getCategory() == MatchExpression::MatchCategory::kLeaf) {
        return (expr.matchType() == MatchExpression::MatchType::EXISTS && expr.path() == path);
    }
    for (size_t i = 0; i < expr.numChildren(); i++) {
        MatchExpression* child = expr.getChild(i);
        if (hasExistencePredicateOnPath(*child, path)) {
            return true;
        }
    }
    return false;
}

bool isSubsetOf(const MatchExpression* lhs, const MatchExpression* rhs) {
    // lhs is the query and rhs is the index.
    invariant(lhs);
    invariant(rhs);

    if (lhs->equivalent(rhs)) {
        return true;
    }

    // $and/$or should be evaluated prior to leaf MatchExpressions. Additionally any recursion
    // should be done through the 'rhs' expression prior to 'lhs'. Swapping the recursion order
    // would cause a comparison like the following to fail as neither the 'a' or 'b' left hand
    // clause would match the $and on the right hand side on their own.
    //     lhs: {a:5, b:5}
    //     rhs: {$or: [{a: 3}, {$and: [{a: 5}, {b: 5}]}]}

    if (rhs->matchType() == MatchExpression::OR) {
        // 'lhs' must match a subset of the documents matched by 'rhs'.
        for (size_t i = 0; i < rhs->numChildren(); i++) {
            if (isSubsetOf(lhs, rhs->getChild(i))) {
                return true;
            }
        }
        return false;
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

    if (lhs->matchType() == MatchExpression::INTERNAL_BUCKET_GEO_WITHIN &&
        rhs->matchType() == MatchExpression::INTERNAL_BUCKET_GEO_WITHIN) {
        const auto* queryMatchExpression =
            static_cast<const InternalBucketGeoWithinMatchExpression*>(lhs);
        const auto* indexMatchExpression =
            static_cast<const InternalBucketGeoWithinMatchExpression*>(rhs);

        // Confirm that the "field" arguments match before continuing.
        if (queryMatchExpression->getField() != indexMatchExpression->getField()) {
            return false;
        }

        GeometryContainer geometry = queryMatchExpression->getGeoContainer();
        if (GeoMatchExpression::contains(
                indexMatchExpression->getGeoContainer(), GeoExpression::WITHIN, &geometry)) {
            // The region described by query is within the region captured by the index.
            // For example, a query over the $geometry for the city of Houston is covered by an
            // index over the $geometry for the entire state of texas. Therefore this index can be
            // used in a potential solution for this query.
            return true;
        }
    }

    if (lhs->matchType() == MatchExpression::GEO && rhs->matchType() == MatchExpression::GEO) {
        // lhs is the query, eg {loc: {$geoWithin: {$geometry: {type: "Polygon", coordinates:
        // [...]}}}} geoWithinObj is {$geoWithin: {$geometry: {type: "Polygon", coordinates:
        // [...]}}} geoWithinElement is '$geoWithin: {$geometry: {type: "Polygon", coordinates:
        // [...]}}' geometryObj is  {$geometry: {type: "Polygon", coordinates: [...]}}
        // geometryElement '$geometry: {type: "Polygon", coordinates: [...]}'

        const auto* queryMatchExpression = static_cast<const GeoMatchExpression*>(lhs);
        // We only handle geoWithin queries
        if (queryMatchExpression->getGeoExpression().getPred() != GeoExpression::WITHIN) {
            return false;
        }
        const auto* indexMatchExpression = static_cast<const GeoMatchExpression*>(rhs);

        auto geometryContainer = queryMatchExpression->getGeoExpression().getGeometry();
        if (indexMatchExpression->matchesGeoContainer(geometryContainer)) {
            // The region described by query is within the region captured by the index.
            // Therefore this index can be used in a potential solution for this query.
            return true;
        }
    }

    if (ComparisonMatchExpression::isComparisonMatchExpression(rhs)) {
        return _isSubsetOf(lhs, static_cast<const ComparisonMatchExpression*>(rhs));
    }

    if (ComparisonMatchExpressionBase::isInternalExprComparison(rhs->matchType())) {
        return _isSubsetOfInternalExpr(lhs, static_cast<const ComparisonMatchExpressionBase*>(rhs));
    }

    if (rhs->matchType() == MatchExpression::EXISTS) {
        return _isSubsetOf(lhs, static_cast<const ExistsMatchExpression*>(rhs));
    }

    if (rhs->matchType() == MatchExpression::MATCH_IN) {
        return _isSubsetOf(lhs, static_cast<const InMatchExpression*>(rhs));
    }

    return false;
}

// Checks if 'expr' has any children which do not have renaming implemented.
bool hasOnlyRenameableMatchExpressionChildren(const MatchExpression& expr) {
    if (expr.matchType() == MatchExpression::MatchType::EXPRESSION) {
        return true;
    } else if (expr.getCategory() == MatchExpression::MatchCategory::kArrayMatching ||
               expr.getCategory() == MatchExpression::MatchCategory::kOther) {
        return false;
    } else if (expr.getCategory() == MatchExpression::MatchCategory::kLogical) {
        for (size_t i = 0; i < expr.numChildren(); i++) {
            if (!hasOnlyRenameableMatchExpressionChildren(*expr.getChild(i))) {
                return false;
            }
        }
    }
    return true;
}

bool containsDependency(const OrderedPathSet& testSet, const OrderedPathSet& prefixCandidates) {
    if (testSet.empty()) {
        return false;
    }

    PathComparator pathComparator;
    auto i2 = testSet.begin();
    for (auto p1 : prefixCandidates) {
        while (pathComparator(*i2, p1)) {
            ++i2;
            if (i2 == testSet.end()) {
                return false;
            }
        }
        // At this point we know that p1 <= *i2, so it may be identical or a path prefix.
        if (p1 == *i2 || isPathPrefixOf(p1, *i2)) {
            return true;
        }
    }
    return false;
}

bool areIndependent(const OrderedPathSet& pathSet1, const OrderedPathSet& pathSet2) {
    return !containsDependency(pathSet1, pathSet2) && !containsDependency(pathSet2, pathSet1);
}

bool isIndependentOf(const MatchExpression& expr, const OrderedPathSet& pathSet) {
    // Any expression types that do not have renaming implemented cannot have their independence
    // evaluated here. See applyRenamesToExpression().
    if (!hasOnlyRenameableMatchExpressionChildren(expr)) {
        return false;
    }

    auto depsTracker = DepsTracker{};
    expr.addDependencies(&depsTracker);
    // Match expressions that generate random numbers can't be safely split out and pushed down.
    if (depsTracker.needRandomGenerator || depsTracker.needWholeDocument) {
        return false;
    }
    return areIndependent(pathSet, depsTracker.fields);
}

bool isOnlyDependentOn(const MatchExpression& expr, const OrderedPathSet& pathSet) {
    // Any expression types that do not have renaming implemented cannot have their independence
    // evaluated here. See applyRenamesToExpression().
    if (!hasOnlyRenameableMatchExpressionChildren(expr)) {
        return false;
    }

    // The approach below takes only O(n log n) time.

    // Find the unique dependencies of pathSet.
    auto pathsDeps =
        DepsTracker::simplifyDependencies(pathSet, DepsTracker::TruncateToRootLevel::no);
    auto pathsDepsCopy = OrderedPathSet(pathsDeps.begin(), pathsDeps.end());

    // Now add the match expression's paths and see if the dependencies are the same.
    auto exprDepsTracker = DepsTracker{};
    expr.addDependencies(&exprDepsTracker);
    // Match expressions that generate random numbers can't be safely split out and pushed down.
    if (exprDepsTracker.needRandomGenerator) {
        return false;
    }
    pathsDepsCopy.insert(exprDepsTracker.fields.begin(), exprDepsTracker.fields.end());

    return pathsDeps ==
        DepsTracker::simplifyDependencies(pathsDepsCopy, DepsTracker::TruncateToRootLevel::no);
}

std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitMatchExpressionBy(
    unique_ptr<MatchExpression> expr,
    const OrderedPathSet& fields,
    const StringMap<std::string>& renames,
    ShouldSplitExprFunc func /*= isIndependentOf */) {
    auto splitExpr = splitMatchExpressionByFunction(std::move(expr), fields, func);
    if (splitExpr.first) {
        applyRenamesToExpression(splitExpr.first.get(), renames);
    }
    return splitExpr;
}

void applyRenamesToExpression(MatchExpression* expr, const StringMap<std::string>& renames) {
    if (expr->matchType() == MatchExpression::MatchType::EXPRESSION) {
        ExprMatchExpression* exprExpr = checked_cast<ExprMatchExpression*>(expr);
        exprExpr->applyRename(renames);
        return;
    }

    if (expr->getCategory() == MatchExpression::MatchCategory::kArrayMatching ||
        expr->getCategory() == MatchExpression::MatchCategory::kOther) {
        return;
    }

    if (expr->getCategory() == MatchExpression::MatchCategory::kLeaf) {
        LeafMatchExpression* leafExpr = checked_cast<LeafMatchExpression*>(expr);
        leafExpr->applyRename(renames);
    }

    for (size_t i = 0; i < expr->numChildren(); ++i) {
        applyRenamesToExpression(expr->getChild(i), renames);
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

bool bidirectionalPathPrefixOf(StringData first, StringData second) {
    return first == second || expression::isPathPrefixOf(first, second) ||
        expression::isPathPrefixOf(second, first);
}

std::pair<StringMap<std::unique_ptr<MatchExpression>>, std::unique_ptr<MatchExpression>>
splitMatchExpressionForColumns(const MatchExpression* me) {
    StringMap<std::unique_ptr<MatchExpression>> out;
    auto residualMatch = mongo::splitMatchExpressionForColumns(me, out);
    return {std::move(out), std::move(residualMatch)};
}

std::string filterMapToString(const StringMap<std::unique_ptr<MatchExpression>>& filterMap) {
    StringBuilder sb;
    sb << "{";
    for (auto&& [path, matchExpr] : filterMap) {
        sb << path << ": " << matchExpr->toString() << ", ";
    }
    sb << "}";
    return sb.str();
}
}  // namespace expression
}  // namespace mongo
