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

#include "mongo/db/matcher/expression_algo.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/exec/matcher/matcher_geo.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <iterator>
#include <set>
#include <type_traits>

#include <s2cellid.h>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
        return cme->getData().type() != BSONType::null;
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
                    return cme->getData().type() == BSONType::null;
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
    const StringMap<std::string>& renames,
    expression::Renameables& renameables,
    expression::ShouldSplitExprFunc shouldSplitOut) {
    if (shouldSplitOut(*expr, fields, renames, renameables)) {
        // 'expr' satisfies our split condition and can be completely split out.
        return {std::move(expr), nullptr};
    }

    // At this point, the content of 'renameables' is no longer applicable because we chose not to
    // proceed with the wholesale extraction of 'expr', or we try to find portion of 'expr' that can
    // be split out by recursing down. In either case, we want to restart our renamable analysis and
    // reset the state.
    renameables.clear();

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
                expression::Renameables childRenameables;
                auto children = splitMatchExpressionByFunction(
                    andExpr->releaseChild(i), fields, renames, childRenameables, shouldSplitOut);

                invariant(children.first || children.second);

                if (children.first) {
                    splitOut.push_back(std::move(children.first));
                    // Accumulate the renameable expressions from the children.
                    renameables.insert(
                        renameables.end(), childRenameables.begin(), childRenameables.end());
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
                expression::Renameables childRenameables;
                auto child = norExpr->releaseChild(i);
                if (shouldSplitOut(*child, fields, renames, childRenameables)) {
                    splitOut.push_back(std::move(child));
                    // Accumulate the renameable expressions from the children.
                    renameables.insert(
                        renameables.end(), childRenameables.begin(), childRenameables.end());
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
        default: {
            MONGO_UNREACHABLE;
        }
    }
}

bool pathDependenciesAreExact(StringData key, const MatchExpression* expr) {
    DepsTracker columnDeps;
    dependency_analysis::addDependencies(expr, &columnDeps);
    return !columnDeps.needWholeDocument && columnDeps.fields == OrderedPathSet{std::string{key}};
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
        return me->clone();

    addExpr(path, me->clone(), out);
    return nullptr;
}

/**
 * Here we check whether the comparison can work with the given value. Objects and arrays are
 * generally not permitted. Objects can't work because the paths will be split apart in the columnar
 * index. We could do arrays of scalars since we would have all that information in the index, but
 * it proved complex to integrate due to the interface with the matcher. It expects to get a
 * BSONElement for the whole Array but we'd like to avoid materializing that.
 *
 * One exception to the above: We can support EQ with empty objects and empty arrays since those are
 * stored as values in CSI. Maybe could also support LT and LTE, but those don't seem as important
 * so are left for future work.
 */
bool canCompareWith(const BSONElement& elem, bool isEQ) {
    const auto type = elem.type();
    if (type == BSONType::minKey || type == BSONType::maxKey) {
        // MinKey and MaxKey have special semantics for comparison to objects.
        return false;
    }
    if (type == BSONType::array || type == BSONType::object) {
        return isEQ && elem.Obj().isEmpty();
    }

    // We support all other types, except null, since it is equivalent to x==null || !exists(x).
    return !elem.isNull();
}

/**
 * Helper for the main public API. Returns the residual predicate and adds any columnar predicates
 * into 'out', if they can be pushed down on their own, or into 'pending' if they can be pushed down
 * only if there are fully supported predicates on the same path.
 */
std::unique_ptr<MatchExpression> splitMatchExpressionForColumns(
    const MatchExpression* me,
    StringMap<std::unique_ptr<MatchExpression>>& out,
    StringMap<std::unique_ptr<MatchExpression>>& pending) {
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
                return me->clone();
            return tryAddExpr(sub->path(), me, out);
        }

        case MatchExpression::MATCH_IN: {
            auto sub = checked_cast<const InMatchExpression*>(me);
            if (sub->hasNonScalarOrNonEmptyValues()) {
                return me->clone();
            }
            return tryAddExpr(sub->path(), me, out);
        }

        case MatchExpression::TYPE_OPERATOR: {
            auto sub = checked_cast<const TypeMatchExpression*>(me);
            tassert(6430600,
                    "Not expecting to find EOO in a $type expression",
                    !sub->typeSet().hasType(BSONType::eoo));
            return tryAddExpr(sub->path(), me, out);
        }

        case MatchExpression::AND: {
            auto originalAnd = checked_cast<const AndMatchExpression*>(me);
            std::vector<std::unique_ptr<MatchExpression>> newChildren;
            for (size_t i = 0, end = originalAnd->numChildren(); i != end; ++i) {
                if (auto residual =
                        splitMatchExpressionForColumns(originalAnd->getChild(i), out, pending)) {
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
            // We can support negation of all supported operators, except AND. The unsupported ops
            // would manifest as non-null residual.
            auto sub = checked_cast<const NotMatchExpression*>(me)->getChild(0);
            if (sub->matchType() == MatchExpression::AND) {
                return me->clone();
            }
            StringMap<std::unique_ptr<MatchExpression>> outSub;
            StringMap<std::unique_ptr<MatchExpression>> pendingSub;
            auto residual = splitMatchExpressionForColumns(sub, outSub, pendingSub);
            if (residual || !pendingSub.empty()) {
                return me->clone();
            }
            uassert(7040600, "Should have exactly one path under $not", outSub.size() == 1);
            return tryAddExpr(outSub.begin()->first /* path */, me, pending);
        }

        // We don't currently handle any of these cases, but some may be possible in the future.
        case MatchExpression::ALWAYS_FALSE:
        case MatchExpression::ALWAYS_TRUE:
        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::ELEM_MATCH_VALUE:  // This one should be feasible. May be valuable.
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
        case MatchExpression::INTERNAL_EQ_HASHED_KEY:
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
            return me->clone();
    }
    MONGO_UNREACHABLE;
}

/**
 * Return true if any of the paths in 'prefixCandidates' are identical to or an ancestor of any
 * of the paths in 'testSet'.  The order of the parameters matters -- it's not commutative. It is
 * important that 'testSet' and 'prefixCandidates' use the same comparator for ordering.
 */
template <typename T>
bool containsDependencyHelper(const std::set<T, PathComparator>& testSet,
                              const OrderedPathSet& prefixCandidates) {
    if (testSet.empty()) {
        return false;
    }

    PathComparator pathComparator;
    auto i2 = testSet.begin();
    for (const auto& p1 : prefixCandidates) {
        while (pathComparator(*i2, p1)) {
            ++i2;
            if (i2 == testSet.end()) {
                return false;
            }
        }
        // At this point we know that p1 <= *i2, so it may be identical or a path prefix.
        if (p1 == *i2 || expression::isPathPrefixOf(p1, *i2)) {
            return true;
        }
    }
    return false;
}

bool hasPredicateOnPathsHelper(const MatchExpression& expr,
                               mongo::MatchExpression::MatchType searchType,
                               const OrderedPathSet& paths,
                               boost::optional<StringData> parentPath) {
    // Accumulate the path components from any ancestors with partial paths (eg. $elemMatch) through
    // the tree to the leaves. Leaf expressions as children of these partial-path expressions will
    // sometimes have no path and would otherwise fail to be considered here.
    std::string ownedPath;
    boost::optional<StringData> fullPath;
    if (expr.fieldRef()) {
        if (parentPath) {
            ownedPath = std::string{*parentPath} + "." + expr.fieldRef()->dottedField();
            fullPath = ownedPath;
        } else {
            fullPath = expr.fieldRef()->dottedField();
        }
    } else {
        fullPath = parentPath;
    }

    if (expr.getCategory() == MatchExpression::MatchCategory::kLeaf && fullPath) {
        return ((expr.matchType() == searchType) &&
                containsDependencyHelper<StringData>({*fullPath}, paths));
    }
    for (size_t i = 0; i < expr.numChildren(); i++) {
        MatchExpression* child = expr.getChild(i);
        if (hasPredicateOnPathsHelper(*child, searchType, paths, fullPath)) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace expression {

bool hasPredicateOnPaths(const MatchExpression& expr,
                         mongo::MatchExpression::MatchType searchType,
                         const OrderedPathSet& paths) {
    return hasPredicateOnPathsHelper(expr, searchType, paths, boost::none /* parentPath */);
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
        // Do not return here and fallthrough to cases below. The LHS may be a $or which requires
        // its own recursive calls to verify.
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
        if (exec::matcher::geoContains(
                indexMatchExpression->getGeoContainer(), GeoExpression::WITHIN, geometry)) {
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
        if (exec::matcher::matchesGeoContainer(indexMatchExpression, geometryContainer)) {
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

// Type requirements for the hashOnlyRenameableMatchExpressionChildrenImpl() & isIndependentOfImpl()
// & isOnlyDependentOnImpl() functions
template <bool IsMutable, typename T>
using MaybeMutablePtr = typename std::conditional<IsMutable, T*, const T*>::type;

// const MatchExpression& should be passed with no 'renameables' argument to traverse the expression
// tree in read-only mode.
template <typename E, typename... Args>
concept ConstTraverseMatchExpression = requires(E&& expr, Args&&... args) {
    sizeof...(Args) == 0 && std::is_same_v<const MatchExpression&, E>;
};

// MatchExpression& should be passed with a single 'renameables' argument to traverse the expression
// tree in read-write mode.
template <typename E, typename... Args>
constexpr bool shouldCollectRenameables = std::is_same_v<MatchExpression&, E> &&
    sizeof...(Args) == 1 && (std::is_same_v<Renameables&, Args> && ...);

// Traversing the expression tree in read-write mode is same as the 'shouldCollectRenameables'.
template <typename E, typename... Args>
concept MutableTraverseMatchExpression = shouldCollectRenameables<E, Args...>;

// We traverse the expression tree in either read-only mode or read-write mode.
template <typename E, typename... Args>
requires ConstTraverseMatchExpression<E, Args...> || MutableTraverseMatchExpression<E, Args...>
bool hasOnlyRenameableMatchExpressionChildrenImpl(E&& expr,
                                                  const StringMap<std::string>& renames,
                                                  Args&&... renameables) {
    constexpr bool mutating = shouldCollectRenameables<E, Args...>;

    if (expr.matchType() == MatchExpression::MatchType::EXPRESSION) {
        if constexpr (mutating) {
            auto exprExpr = checked_cast<MaybeMutablePtr<mutating, ExprMatchExpression>>(&expr);
            if (renames.size() > 0 && exprExpr->hasRenameablePath(renames)) {
                // The second element is ignored for $expr.
                (renameables.emplace_back(exprExpr, ""_sd), ...);
            }
        }

        return true;
    }

    if (expr.getCategory() == MatchExpression::MatchCategory::kOther) {
        if constexpr (mutating) {
            (renameables.clear(), ...);
        }
        return false;
    }

    if (expr.getCategory() == MatchExpression::MatchCategory::kArrayMatching ||
        expr.getCategory() == MatchExpression::MatchCategory::kLeaf) {
        auto pathExpr = checked_cast<MaybeMutablePtr<mutating, PathMatchExpression>>(&expr);
        if (renames.size() == 0 || !pathExpr->optPath()) {
            return true;
        }

        // Cannot proceed to dependency or independence checks if any attempted rename would fail.
        auto&& [wouldSucceed, optNewPath] = pathExpr->wouldRenameSucceed(renames);
        if (!wouldSucceed) {
            if constexpr (mutating) {
                (renameables.clear(), ...);
            }
            return false;
        }

        if constexpr (mutating) {
            if (optNewPath) {
                (renameables.emplace_back(pathExpr, *optNewPath), ...);
            }
        }

        return true;
    }

    tassert(7585300,
            "Expression category must be logical at this point",
            expr.getCategory() == MatchExpression::MatchCategory::kLogical);
    for (size_t i = 0; i < expr.numChildren(); ++i) {
        bool hasOnlyRenameables = [&] {
            if constexpr (mutating) {
                return (hasOnlyRenameableMatchExpressionChildrenImpl(
                            *(expr.getChild(i)), renames, std::forward<Args>(renameables)),
                        ...);
            } else {
                return hasOnlyRenameableMatchExpressionChildrenImpl(*(expr.getChild(i)), renames);
            }
        }();
        if (!hasOnlyRenameables) {
            if constexpr (mutating) {
                (renameables.clear(), ...);
            }
            return false;
        }
    }

    return true;
}

bool hasOnlyRenameableMatchExpressionChildren(MatchExpression& expr,
                                              const StringMap<std::string>& renames,
                                              Renameables& renameables) {
    return hasOnlyRenameableMatchExpressionChildrenImpl(expr, renames, renameables);
}

bool hasOnlyRenameableMatchExpressionChildren(const MatchExpression& expr,
                                              const StringMap<std::string>& renames) {
    return hasOnlyRenameableMatchExpressionChildrenImpl(expr, renames);
}

bool containsDependency(const OrderedPathSet& testSet, const OrderedPathSet& prefixCandidates) {
    return containsDependencyHelper(testSet, prefixCandidates);
}

bool containsOverlappingPaths(const OrderedPathSet& testSet) {
    // We will take advantage of the fact that paths with common ancestors are ordered together in
    // our ordering. Thus if there are any paths that contain a common ancestor, they will be right
    // next to each other - unless there are multiple pairs, in which case at least one pair will be
    // right next to each other.
    if (testSet.empty()) {
        return false;
    }
    for (auto it = std::next(testSet.begin()); it != testSet.end(); ++it) {
        if (isPathPrefixOf(*std::prev(it), *it)) {
            return true;
        }
    }
    return false;
}

bool containsEmptyPaths(const OrderedPathSet& testSet) {
    return std::any_of(testSet.begin(), testSet.end(), [](const auto& path) {
        if (path.empty()) {
            return true;
        }

        FieldRef fieldRef(path);

        for (size_t i = 0; i < fieldRef.numParts(); ++i) {
            if (fieldRef.getPart(i).empty()) {
                return true;
            }
        }

        // all non-empty
        return false;
    });
}


bool areIndependent(const OrderedPathSet& pathSet1, const OrderedPathSet& pathSet2) {
    return !containsDependency(pathSet1, pathSet2) && !containsDependency(pathSet2, pathSet1);
}

OrderedPathSet makeIndependent(OrderedPathSet testSet, const OrderedPathSet& toRemove) {
    auto testItr = testSet.begin();
    auto removalItr = toRemove.begin();

    ThreeWayPathComparator comp;

    while (testItr != testSet.end() && removalItr != toRemove.end()) {
        const auto& path = *testItr;
        const auto& removePath = *removalItr;

        const auto res = comp(path, removePath);

        if (std::is_lt(res)) {
            // The currently considered path sorts before the current removePath.
            // Therefore, it either doesn't match the path, or is a prefix.
            if (isPathPrefixOf(path, removePath)) {
                // `path` prefixes a path in toRemove. To make the sets independent,
                // `path` must be erased.
                // `removePath` may match more elements in `testSet`.
                testItr = testSet.erase(testItr);
            } else {
                // `path` < `removePath`, but `path` is not a prefix of `removePath`.
                // `toRemove` is sorted, so no later element of `toRemove` can match `path`.
                // Thus, `path` should remain in `testSet`.
                // `removePath` may match later elements in `testSet`.
                ++testItr;
            }
        } else if (std::is_gt(res)) {
            // `removePath` sorts _before_ the current path.
            // `path` either prefixes `removePath`, or is unrelated.
            if (isPathPrefixOf(removePath, path)) {
                // A path in toRemove prefixes `path`. To make the sets independent,
                // `path` must be erased.
                // `removePath` may match more elements in `testSet`.
                testItr = testSet.erase(testItr);
            } else {
                // `path` > `removePath`, but `path` is not prefixed by `removePath`.
                // `path` _may_ match a later path in `toRemove`, so advance to the next
                // element of `toRemove`.
                // This is safe, as `removePath` can't match any later elements in `testSet`.
                ++removalItr;
            }
        } else {
            // !(a < b) && !(b < a) => a == b
            // There is an exact matching path in `toRemove`.
            // Remove it from `testSet`.
            // `removePath` may match more elements in `testSet`.
            testItr = testSet.erase(testItr);
        }
    }

    return testSet;
}

template <typename E, typename... Args>
requires ConstTraverseMatchExpression<E, Args...> || MutableTraverseMatchExpression<E, Args...>
bool isIndependentOfImpl(E&& expr,
                         const OrderedPathSet& pathSet,
                         const StringMap<std::string>& renames,
                         Args&&... renameables) {
    constexpr bool mutating = shouldCollectRenameables<E, Args...>;

    if (expr.getCategory() == MatchExpression::MatchCategory::kLogical) {
        // The whole expression is independent of 'pathSet' if and only if every child is.
        for (int i = 0, numChildren = expr.numChildren(); i < numChildren; ++i) {
            if (!isIndependentOfImpl<E, Args...>(
                    *expr.getChild(i), pathSet, renames, renameables...)) {
                return false;
            }
        }
        return true;
    }

    // Any expression types that do not have renaming implemented cannot have their independence
    // evaluated here. See applyRenamesToExpression().
    bool hasOnlyRenameables = [&] {
        if constexpr (mutating) {
            return (hasOnlyRenameableMatchExpressionChildrenImpl(
                        expr, renames, std::forward<Args>(renameables)),
                    ...);
        } else {
            return hasOnlyRenameableMatchExpressionChildrenImpl(expr, renames);
        }
    }();

    if (!hasOnlyRenameables) {
        return false;
    }

    auto depsTracker = DepsTracker{};
    dependency_analysis::addDependencies(&expr, &depsTracker);
    // Match expressions that generate random numbers can't be safely split out and pushed down.
    if (depsTracker.needRandomGenerator || depsTracker.needWholeDocument) {
        return false;
    }

    // When the paths diverge but share a nonempty prefix, they may or may
    // not be independent: it depends on the details of the match predicate.
    const bool canHaveSharedPrefix = [&] {
        if (expr.matchType() == MatchExpression::EXPRESSION) {
            // We assume any dependencies within $expr use ExpressionFieldPath, which
            // is not affected when prefixes of its path change from scalar to object.
            // See 'jstests/aggregation/sources/addFields/independence.js'.
            return true;
        }

        // The most typical match expression uses a predicate like '$eq',
        // whose non-leaf behavior traverses arrays. Typically the path is not numeric
        // and the predicate is false on a missing field. When all these conditions are met,
        // an $addFields on a diverging path won't affect the predicate result.
        if (auto* pathMatch = dynamic_cast<const PathMatchExpression*>(&expr)) {
            const auto kTraverse = ElementPath::NonLeafArrayBehavior::kTraverse;
            return
                // Has the typical array behavior.
                (pathMatch->elementPath()->nonLeafArrayBehavior() == kTraverse)
                // No numeric components.
                && !pathMatch->elementPath()->fieldRef().hasNumericPathComponents()
                // Ignores missing fields.
                &&
                !exec::matcher::matchesSingleElement(pathMatch, BSONObj{}.firstElement(), nullptr);
        }

        // Other cases may be allowable, but haven't been considered and tested yet.
        return false;
    }();

    if (canHaveSharedPrefix) {
        return areIndependent(pathSet, depsTracker.fields);
    } else {
        // All paths must diverge on the first component.
        OrderedPathSet truncated;
        for (StringData path : pathSet) {
            if (size_t dotPos = path.find('.'); dotPos != std::string::npos) {
                path = path.substr(0, dotPos);
            }
            if (auto it = truncated.find(path); it == truncated.end()) {
                truncated.insert(std::string{path});
            }
        }
        return areIndependent(truncated, depsTracker.fields);
    }
}

bool isIndependentOf(MatchExpression& expr,
                     const OrderedPathSet& pathSet,
                     const StringMap<std::string>& renames,
                     Renameables& renameables) {
    return isIndependentOfImpl(expr, pathSet, renames, renameables);
}

bool isIndependentOfConst(const MatchExpression& expr,
                          const OrderedPathSet& pathSet,
                          const StringMap<std::string>& renames) {
    return isIndependentOfImpl(expr, pathSet, renames);
}

template <typename E, typename... Args>
requires ConstTraverseMatchExpression<E, Args...> || MutableTraverseMatchExpression<E, Args...>
bool isOnlyDependentOnImpl(E&& expr,
                           const OrderedPathSet& pathSet,
                           const StringMap<std::string>& renames,
                           Args&&... renameables) {
    constexpr bool mutating = shouldCollectRenameables<E, Args...>;

    // Any expression types that do not have renaming implemented cannot have their independence
    // evaluated here. See applyRenamesToExpression().
    bool hasOnlyRenameables = [&] {
        if constexpr (mutating) {
            return (hasOnlyRenameableMatchExpressionChildrenImpl(
                        expr, renames, std::forward<Args>(renameables)),
                    ...);
        } else {
            return hasOnlyRenameableMatchExpressionChildrenImpl(expr, renames);
        }
    }();

    // Any expression types that do not have renaming implemented cannot have their independence
    // evaluated here. See applyRenamesToExpression().
    if (!hasOnlyRenameables) {
        return false;
    }

    // The approach below takes only O(n log n) time.

    // Find the unique dependencies of pathSet.
    auto pathsDeps =
        DepsTracker::simplifyDependencies(pathSet, DepsTracker::TruncateToRootLevel::no);
    auto pathsDepsCopy = OrderedPathSet(pathsDeps.begin(), pathsDeps.end());

    // Now add the match expression's paths and see if the dependencies are the same.
    auto exprDepsTracker = DepsTracker{};
    dependency_analysis::addDependencies(&expr, &exprDepsTracker);
    // Match expressions that generate random numbers can't be safely split out and pushed down.
    if (exprDepsTracker.needRandomGenerator) {
        return false;
    }
    pathsDepsCopy.insert(exprDepsTracker.fields.begin(), exprDepsTracker.fields.end());

    return pathsDeps ==
        DepsTracker::simplifyDependencies(std::move(pathsDepsCopy),
                                          DepsTracker::TruncateToRootLevel::no);
}

bool isOnlyDependentOn(MatchExpression& expr,
                       const OrderedPathSet& pathSet,
                       const StringMap<std::string>& renames,
                       Renameables& renameables) {
    return isOnlyDependentOnImpl(expr, pathSet, renames, renameables);
}

bool isOnlyDependentOnConst(const MatchExpression& expr,
                            const OrderedPathSet& pathSet,
                            const StringMap<std::string>& renames) {
    return isOnlyDependentOnImpl(expr, pathSet, renames);
}

std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitMatchExpressionBy(
    unique_ptr<MatchExpression> expr,
    const OrderedPathSet& fields,
    const StringMap<std::string>& renames,
    ShouldSplitExprFunc func /*= isIndependentOf */) {
    Renameables renameables;
    auto splitExpr =
        splitMatchExpressionByFunction(std::move(expr), fields, renames, renameables, func);
    if (splitExpr.first && !renames.empty()) {
        applyRenamesToExpression(renames, &renameables);
    }
    return splitExpr;
}

void applyRenamesToExpression(const StringMap<std::string>& renames,
                              const Renameables* renameables) {
    tassert(7585301, "Invalid argument", renameables);
    for (auto&& [matchExpr, newPath] : *renameables) {
        if (holds_alternative<PathMatchExpression*>(matchExpr)) {
            // PathMatchExpression.
            get<PathMatchExpression*>(matchExpr)->setPath(newPath);
        } else {
            // ExprMatchExpression.
            get<ExprMatchExpression*>(matchExpr)->applyRename(renames);
        }
    }
}

std::unique_ptr<MatchExpression> copyExpressionAndApplyRenames(
    const MatchExpression* expr, const StringMap<std::string>& renames) {
    Renameables renameables;
    if (auto exprCopy = expr->clone();
        hasOnlyRenameableMatchExpressionChildren(*exprCopy, renames, renameables)) {
        applyRenamesToExpression(renames, &renameables);
        return exprCopy;
    } else {
        return nullptr;
    }
}

void mapOver(MatchExpression* expr, NodeTraversalFunc func, std::string path) {
    if (!expr->path().empty()) {
        if (!path.empty()) {
            path += ".";
        }

        path += std::string{expr->path()};
    }

    for (size_t i = 0; i < expr->numChildren(); i++) {
        mapOver(expr->getChild(i), func, path);
    }

    func(expr, path);
}
namespace {
/**
 * Helper function for assumeImpreciseInternalExprNodesReturnTrue(). Given a tree-like
 * match expression (one which can have multiple children e.g. AND, OR, NOR), walk it and
 * apply the assumeImpreciseInternalExprNodesReturnTrue() to each child.
 *
 * If a child is trivially true or trivially false, the expression is simplified based on the
 * callbacks onTriviallyTrue() and onTriviallyFalse(). A return of nullptr from the callback
 * indicates that the node should be removed, and a return value of non-null indicates that the
 * non-null value should replace the entire match expression.
 */
std::unique_ptr<MatchExpression> rewriteTreeNode(
    std::unique_ptr<MatchExpression> exprOwned,
    const std::function<std::unique_ptr<MatchExpression>(std::unique_ptr<MatchExpression>)>&
        onTriviallyTrue,
    const std::function<std::unique_ptr<MatchExpression>(std::unique_ptr<MatchExpression>)>&
        onTriviallyFalse) {

    auto* listOfNode = static_cast<ListOfMatchExpression*>(exprOwned.get());
    size_t i = 0;
    size_t nChildren = exprOwned->numChildren();

    while (i < nChildren) {
        auto& node = (*listOfNode->getChildVector())[i];
        auto newNode = assumeImpreciseInternalExprNodesReturnTrue(std::move(node));
        if (newNode->isTriviallyTrue()) {
            if (auto ret = onTriviallyTrue(std::move(newNode)); ret) {
                return ret;
            } else {
                listOfNode->removeChild(i);
                nChildren--;
            }
        } else if (newNode->isTriviallyFalse()) {
            if (auto ret = onTriviallyFalse(std::move(newNode)); ret) {
                return ret;
            } else {
                listOfNode->removeChild(i);
                nChildren--;
            }
        } else {
            (*listOfNode->getChildVector())[i] = std::move(newNode);
            ++i;
        }
    }
    return exprOwned;
}
}  // namespace

std::unique_ptr<MatchExpression> assumeImpreciseInternalExprNodesReturnTrue(
    std::unique_ptr<MatchExpression> exprOwned) {
    auto matchType = exprOwned->matchType();
    auto expr = exprOwned.get();

    if (matchType == MatchExpression::AND) {
        return rewriteTreeNode(
            std::move(exprOwned),
            // Remove any trivially true node.
            [](auto trueNode) { return nullptr; },
            // If any false node is found, that becomes the new root.
            [](auto falseNode) { return falseNode; });

    } else if (matchType == MatchExpression::OR) {
        return rewriteTreeNode(
            std::move(exprOwned),
            // Any trivially true node makes this OR true.
            [](auto trueNode) { return trueNode; },
            // If any false node is found, remove it.
            [](auto falseNode) { return nullptr; });

    } else if (matchType == MatchExpression::NOR) {
        return rewriteTreeNode(
            std::move(exprOwned),
            // Any trivially true node makes this entire node false.
            [](auto trueNode) { return std::make_unique<AlwaysFalseMatchExpression>(); },
            // If any false node is found, remove it.
            [](auto falseNode) { return nullptr; });
    } else if (ComparisonMatchExpressionBase::isInternalExprComparison(expr->matchType())) {
        return std::make_unique<AlwaysTrueMatchExpression>();
    } else {
        return exprOwned;
    }
}

bool isPathPrefixOf(StringData first, StringData second) {
    if (first.size() >= second.size()) {
        return false;
    }

    return second.starts_with(first) && second[first.size()] == '.';
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
