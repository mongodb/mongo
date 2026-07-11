// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Logic for how indices can be used with an expression.
 */
class Indexability {
public:
    /**
     * Is an index over me->path() useful?
     * This is the same thing as being sargable, if you have a RDBMS background.
     */
    static bool nodeCanUseIndexOnOwnField(const MatchExpression* me) {
        if (me->path().empty()) {
            return false;
        }

        if (arrayUsesIndexOnOwnField(me)) {
            return true;
        }

        return isIndexOnOwnFieldTypeNode(me);
    }

    /**
     * Type bracketing does not apply to internal Expressions. This could cause the use of a sparse
     * index return incomplete results. For example, a query {$expr: {$lt: ["$missing", "r"]}} would
     * expect a document like, {a: 1}, with field "missing" missing be returned. However, a sparse
     * index, {missing: 1} does not index the document. Therefore, we should ban use of any sparse
     * index on following expression types.
     */
    static bool nodeSupportedBySparseIndex(const MatchExpression* me) {
        switch (me->matchType()) {
            case MatchExpression::INTERNAL_EXPR_EQ:
            case MatchExpression::INTERNAL_EXPR_GT:
            case MatchExpression::INTERNAL_EXPR_GTE:
            case MatchExpression::INTERNAL_EXPR_LT:
            case MatchExpression::INTERNAL_EXPR_LTE:
            case MatchExpression::EXPRESSION:
                return false;
            default:
                return true;
        }
    }

    /**
     * Some node types, such as MOD, REGEX, TYPE_OPERATOR, or ELEM_MATCH_VALUE, cannot use index if
     * they are under negation.
     */
    static bool nodeCannotUseIndexUnderNot(const MatchExpression* me) {
        switch (me->matchType()) {
            case MatchExpression::REGEX:
            case MatchExpression::MOD:
            case MatchExpression::TYPE_OPERATOR:
            case MatchExpression::ELEM_MATCH_VALUE:
            case MatchExpression::GEO:
            case MatchExpression::GEO_NEAR:
            case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
                return true;
            case MatchExpression::INTERNAL_EXPR_EQ:
            case MatchExpression::INTERNAL_EXPR_LT:
            case MatchExpression::INTERNAL_EXPR_GTE:
                // Inexact bounds can't be inverted. These comparisons produce inexact bounds due to
                // the lack of type bracketing and indexes not distinguishing between null and
                // missing.
                return !isExactBoundsGenerating(
                    static_cast<const ComparisonMatchExpressionBase*>(me)->getData());
            default:
                return false;
        }
    }

    /**
     * This array operator doesn't have any children with fields and can use an index.
     *
     * Example: a: {$elemMatch: {$gte: 1, $lte: 1}}.
     */
    static bool arrayUsesIndexOnOwnField(const MatchExpression* me) {
        if (me->getCategory() != MatchExpression::MatchCategory::kArrayMatching) {
            return false;
        }

        if (MatchExpression::ELEM_MATCH_VALUE != me->matchType()) {
            return false;
        }

        // We have an ELEM_MATCH_VALUE expression. In order to be
        // considered "indexable" all children of the ELEM_MATCH_VALUE
        // must be "indexable" type expressions as well.
        for (size_t i = 0; i < me->numChildren(); i++) {
            MatchExpression* child = me->getChild(i);

            // Special case for NOT: If the child is a NOT, then it's the thing below
            // the NOT that we care about.
            if (MatchExpression::NOT == child->matchType()) {
                MatchExpression* notChild = child->getChild(0);

                if (MatchExpression::MOD == notChild->matchType() ||
                    MatchExpression::REGEX == notChild->matchType() ||
                    MatchExpression::TYPE_OPERATOR == notChild->matchType()) {
                    // We can't index negations of this kind of expression node.
                    return false;
                }

                // It's the child of the NOT that we check for indexability.
                if (!isIndexOnOwnFieldTypeNode(notChild)) {
                    return false;
                }

                // Special handling for NOT has already been done; don't fall through.
                continue;
            }

            if (!isIndexOnOwnFieldTypeNode(child)) {
                return false;
            }
        }

        // The entire ELEM_MATCH_VALUE is indexable since every one of its children
        // is indexable.
        return true;
    }

    /**
     * Certain array operators require that the field for that operator is prepended
     * to all fields in that operator's children.
     *
     * Example: a: {$elemMatch: {b:1, c:1}}.
     */
    static bool arrayUsesIndexOnChildren(const MatchExpression* me) {
        return MatchExpression::ELEM_MATCH_OBJECT == me->matchType();
    }

    /**
     * Returns true if 'me' is ELEM_MATCH_OBJECT and has non-empty path component.
     *
     * Note: we skip empty path components since they are not allowed in index key patterns.
     * Therefore, $elemMatch with an empty path component can never use an index.
     *
     * Example: {"": {$elemMatch: {a: "hi", b: "bye"}}.
     * In this case the predicate cannot use any indexes since the $elemMatch is with an empty path
     * component.
     */
    static bool isBoundsGeneratingElemMatchObject(const MatchExpression* me) {
        return arrayUsesIndexOnChildren(me) && !me->path().empty();
    }

    /**
     * Returns true if 'me' is a NOT, and the child of the NOT can use
     * an index on its own field.
     */
    static bool isBoundsGeneratingNot(const MatchExpression* me) {
        return MatchExpression::NOT == me->matchType() &&
            nodeCanUseIndexOnOwnField(me->getChild(0));
    }

    /**
     * Returns true if either 'me' is a bounds generating NOT,
     * or 'me' can use an index on its own field.
     */
    static bool isBoundsGenerating(const MatchExpression* me) {
        return isBoundsGeneratingNot(me) || nodeCanUseIndexOnOwnField(me);
    }

    /**
     * Returns true if 'elt' is a BSONType for which exact index bounds can be generated.
     */
    static bool isExactBoundsGenerating(BSONElement elt) {
        switch (elt.type()) {
            case BSONType::numberLong:
            case BSONType::numberDouble:
            case BSONType::numberInt:
            case BSONType::numberDecimal:
            case BSONType::string:
            case BSONType::boolean:
            case BSONType::date:
            case BSONType::timestamp:
            case BSONType::oid:
            case BSONType::binData:
            case BSONType::object:
            case BSONType::code:
            case BSONType::codeWScope:
            case BSONType::minKey:
            case BSONType::maxKey:
                return true;
            default:
                return false;
        }
    }

    /**
     * Check if this match expression is a leaf and is supported by a hashed index.
     */
    static bool nodeIsSupportedByHashedIndex(const MatchExpression* queryExpr) {
        // Hashed fields can answer simple equality predicates.
        if (ComparisonMatchExpressionBase::isEquality(queryExpr->matchType())) {
            return true;
        }
        if (queryExpr->matchType() == MatchExpression::INTERNAL_EQ_HASHED_KEY) {
            return true;
        }
        // An $in can be answered so long as its operand contains only simple equalities.
        if (queryExpr->matchType() == MatchExpression::MATCH_IN) {
            const InMatchExpression* expr = static_cast<const InMatchExpression*>(queryExpr);
            return expr->getRegexes().empty();
        }
        // {$exists:false} produces a single point-interval index bound on [null,null].
        if (queryExpr->matchType() == MatchExpression::NOT) {
            return queryExpr->getChild(0)->matchType() == MatchExpression::EXISTS;
        }
        // {$exists:true} can be answered using [MinKey, MaxKey] bounds.
        return (queryExpr->matchType() == MatchExpression::EXISTS);
    }

    static bool canUseIndexForNin(const InMatchExpression* ime) {
        return !ime->hasRegex() && ime->getEqualities().size() == 2 && ime->hasNull() &&
            ime->hasEmptyArray();
    }

    static bool nodeIsNegationOrElemMatchObj(const MatchExpression* node) {
        return (node->matchType() == MatchExpression::NOT ||
                node->matchType() == MatchExpression::NOR ||
                node->matchType() == MatchExpression::ELEM_MATCH_OBJECT);
    }

private:
    /**
     * Returns true if 'me' is "sargable" but is not a negation and
     * is not an array node such as ELEM_MATCH_VALUE.
     *
     * Used as a helper for nodeCanUseIndexOnOwnField().
     */
    static bool isIndexOnOwnFieldTypeNode(const MatchExpression* me) {
        switch (me->matchType()) {
            case MatchExpression::LTE:
            case MatchExpression::LT:
            case MatchExpression::EQ:
            case MatchExpression::GT:
            case MatchExpression::GTE:
            case MatchExpression::REGEX:
            case MatchExpression::MOD:
            case MatchExpression::MATCH_IN:
            case MatchExpression::TYPE_OPERATOR:
            case MatchExpression::GEO:
            case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
            case MatchExpression::GEO_NEAR:
            case MatchExpression::EXISTS:
            case MatchExpression::TEXT:
            case MatchExpression::INTERNAL_EXPR_EQ:
            case MatchExpression::INTERNAL_EXPR_GT:
            case MatchExpression::INTERNAL_EXPR_GTE:
            case MatchExpression::INTERNAL_EXPR_LT:
            case MatchExpression::INTERNAL_EXPR_LTE:
            case MatchExpression::INTERNAL_EQ_HASHED_KEY:
            case MatchExpression::EXPRESSION:
                return true;
            default:
                return false;
        }
    }
};

}  // namespace mongo
