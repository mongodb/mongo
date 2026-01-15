/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_rewrites.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"

namespace mongo::cost_based_ranker {

namespace {

/* Returns true if the given value, with the given inclusivity,
 * corresponds to the beginning of a type bracket.
 * The logic is derived by inspecting index ranges built for
 * $type queries, eg db.a.explain().find({a: {$type: 'bool'}})
 * where the field in question has an index.
 */
bool isTypeBracketLowerBound(const BSONElement& value, bool isInclusive) {
    if (!isInclusive)
        return false;
    switch (value.type()) {
        case BSONType::numberDouble:
            return std::isnan(value.numberDouble());
        case BSONType::string:
            return value.valuestrsize() == 1;
        case BSONType::object:
            return value.embeddedObject().isEmpty();
        case BSONType::binData:
            return value.binDataType() == BinDataType::BinDataGeneral && value.valuestrsize() == 0;
        case BSONType::undefined:
            return true;
        case BSONType::oid:
            return value.OID() == OID();
        case BSONType::boolean:
            return value.boolean() == false;
        case BSONType::date:
            return value.date() == Date_t::min();
        case BSONType::null:
            return true;
        case BSONType::regEx:
            return value.regex()[0] == '\0' && value.regexFlags()[0] == '\0';
        case BSONType::dbRef:
            return value.dbrefNS()[0] == '\0' && value.dbrefOID() == OID();
        case BSONType::code:
            return value.valuestrsize() == 1;
        case BSONType::codeWScope:
            return value.codeWScopeCodeLen() == 1 && value.codeWScopeObject().isEmpty();
        case BSONType::timestamp:
            return value.timestamp() == Timestamp::min();
        case BSONType::minKey:
        case BSONType::maxKey:
            return true;
        case BSONType::array:
        case BSONType::symbol:
        case BSONType::numberInt:
        case BSONType::numberLong:
        case BSONType::numberDecimal:
        case BSONType::eoo:
            return false;
    }

    MONGO_UNREACHABLE_TASSERT(4793095);
}

/* Returns true if the given value, with the given inclusivity,
 * corresponds to the end of a type bracket.
 * See isLowerBound for details.
 */
bool isTypeBracketUpperBound(const BSONElement& value, bool isInclusive) {
    switch (value.type()) {
        case BSONType::numberDouble:
            return value.numberDouble() == std::numeric_limits<double>::infinity() && isInclusive;
        case BSONType::object:
        case BSONType::array:
            return value.embeddedObject().isEmpty() && !isInclusive;
        case BSONType::undefined:
            return isInclusive;
        case BSONType::oid:
            return (value.OID() == OID() && !isInclusive) ||
                (value.OID() == OID::max() && isInclusive);
        case BSONType::boolean:
            return value.boolean() && isInclusive;
        case BSONType::date:
            return value.date() == Date_t::max() && isInclusive;
        case BSONType::null:
            return isInclusive;
        case BSONType::dbRef:
            return value.dbrefNS()[0] == '\0' && value.dbrefOID() == OID() && !isInclusive;
        case BSONType::code:
            return value.valuestrsize() == 1 && !isInclusive;
        case BSONType::codeWScope:
            return value.codeWScopeCodeLen() == 1 && value.codeWScopeObject().isEmpty() &&
                !isInclusive;
        case BSONType::timestamp:
            return value.timestamp() == Timestamp::max() && isInclusive;
        case BSONType::minKey:
            return isInclusive;
        case BSONType::maxKey:
            return true;
        case BSONType::string:
        case BSONType::binData:
        case BSONType::regEx:
        case BSONType::symbol:
        case BSONType::numberInt:
        case BSONType::numberLong:
        case BSONType::numberDecimal:
        case BSONType::eoo:
            return false;
    }

    MONGO_UNREACHABLE_TASSERT(40957476);
}

const BSONObj constantHolder = BSON_ARRAY(BSONArray() << BSONNULL);
const BSONElement emptyArrayElem = constantHolder["0"];
const BSONElement nullElem = constantHolder["1"];

/**
 * Given a 'path' and an 'interval' on that path, generate a minimal logically equivalent
 * MatchExpression. In principle the expression could be less strict than the interval because it
 * is used for CE, however currently it is strictly equivalent.
 * Example:
 * Consider an open range condition such as {a: {$gt: 42}} and an index on 'a'. This results in an
 * interval [42, inf]. During CE of this interval CBR ends up calling this function to convert it to
 * an equivalent condition, ideally the same one that was used to generate the interval.
 * This function implements careful handling of interval bounds in order to avoid adding
 * logically redundant terms such as the comparison to inf as in this logically equivalent
 * expression: {$and: [{a: {$gt: 42}}, {a: {$lt: inf}}]}
 */
std::unique_ptr<MatchExpression> getMatchExpressionFromInterval(StringData path,
                                                                const Interval& interval) {
    if (interval.isFullyOpen()) {
        // Intervals containing all values of a field can be estimated as True match expression.
        return std::make_unique<AlwaysTrueMatchExpression>();
    }

    if (interval.isNull()) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    if (interval.isPoint()) {
        const BSONType type = interval.start.type();
        if (type == BSONType::undefined) {
            // The range [undefined, undefined] can be derived from either:
            //   - a check for undefined itself (eg. {$type: 'undefined'}) or
            //   - a check for an empty array (eg. {$eq: []}).
            // Both of these predicates remain as the residual filter,
            // so even though we emit a $or here, the disjunction will get
            // normalized away with the call to normalizeMatchExpression.
            std::vector<std::unique_ptr<MatchExpression>> vec;
            vec.push_back(
                std::make_unique<TypeMatchExpression>(path, MatcherTypeSet(BSONType::undefined)));
            vec.push_back(std::make_unique<EqualityMatchExpression>(path, emptyArrayElem));
            return std::make_unique<OrMatchExpression>(std::move(vec));
        } else if (type == BSONType::null) {
            // This is semantically equivalent to just {$eq: null} but
            // gets normalized better by the call to normalizeMatchExpression.
            // Both {$eq: null} and {$exists: false} remain as the residual filter,
            // so even though we emit a $or here, the disjunction will get
            // normalized away with the call to normalizeMatchExpression.
            std::vector<std::unique_ptr<MatchExpression>> vec;
            vec.push_back(std::make_unique<EqualityMatchExpression>(path, nullElem));
            vec.push_back(std::make_unique<NotMatchExpression>(
                std::make_unique<ExistsMatchExpression>(path)));
            return std::make_unique<OrMatchExpression>(std::move(vec));
        }
        // In all other cases, we can emit an equality check.
        return std::make_unique<EqualityMatchExpression>(path, interval.start);
    }

    // Create other comparison expressions.
    auto direction = interval.getDirection();
    tassert(10450101,
            "Expected interval with ascending or descending direction",
            direction != Interval::Direction::kDirectionNone);

    bool isAscending = (direction == Interval::Direction::kDirectionAscending);

    bool gtIncl = (isAscending) ? interval.startInclusive : interval.endInclusive;
    auto& gtVal = (isAscending) ? interval.start : interval.end;
    bool isLB = isTypeBracketLowerBound(gtVal, gtIncl);  // Low type bracket or -inf

    bool ltIncl = (isAscending) ? interval.endInclusive : interval.startInclusive;
    auto& ltVal = (isAscending) ? interval.end : interval.start;
    bool isUB = isTypeBracketUpperBound(ltVal, ltIncl);  // Upper type bracket or inf

    // If this is a type bracket interval create the corresponding $type predicate.
    if (isLB && isUB) {
        const BSONType type = gtVal.type();
        // This assumes we never get index bounds spanning over multiple
        // data types, which does not hold for eg. $type: ['string', 'object'].
        MatcherTypeSet typeSet;
        if (type == BSONType::numberDouble) {
            // We cannot distinguish between numeric types using
            // index bounds alone.
            typeSet.allNumbers = true;
        } else if (type == BSONType::string) {
            // Indices encode strings and symbols identically,
            // so we cannot distinguish between the two by index
            // bounds alone.
            std::vector<std::unique_ptr<MatchExpression>> vec;
            vec.push_back(std::make_unique<TypeMatchExpression>(path, BSONType::string));
            vec.push_back(std::make_unique<TypeMatchExpression>(path, BSONType::symbol));
            // Combining $type predicates with a $or lets the disjunction
            // get normalized away by the normalizeMatchExpression call.
            // See the path for BSONType::undefined
            return std::make_unique<OrMatchExpression>(std::move(vec));
        } else {
            typeSet.bsonTypes.insert(gtVal.type());
        }
        return std::make_unique<TypeMatchExpression>(path, std::move(typeSet));
    }

    std::vector<std::unique_ptr<MatchExpression>> expressions;

    if (isUB ||
        (!isLB &&
         (gtVal.type() != BSONType::numberDouble ||
          gtVal.numberDouble() != -std::numeric_limits<double>::infinity()))) {
        // Create an expression for the lower bound only if it is not minimal for the type. This
        // avoids adding redundant always true conditions such as {a: {$gt: -inf}}.
        if (gtIncl) {
            expressions.push_back(std::make_unique<GTEMatchExpression>(path, gtVal));
        } else {
            expressions.push_back(std::make_unique<GTMatchExpression>(path, gtVal));
        }
    }

    if (!isUB) {
        // Create an expression for the upper bound only if it is not maximal for the type. This
        // avoids adding redundant always true conditions such as {a: {$lt: inf}}.
        if (ltIncl) {
            expressions.push_back(std::make_unique<LTEMatchExpression>(path, ltVal));
        } else {
            expressions.push_back(std::make_unique<LTMatchExpression>(path, ltVal));
        }
    }

    if (expressions.size() > 1) {
        return std::make_unique<AndMatchExpression>(std::move(expressions));
    }

    return std::move(expressions[0]);
}

std::unique_ptr<MatchExpression> getMatchExpressionFromOIL(const OrderedIntervalList* oil) {
    if (oil->isFullyOpen()) {
        // Do not create expression for intervals containing all values of a field.
        return std::make_unique<AlwaysTrueMatchExpression>();
    }

    if (oil->intervals.size() == 0) {
        // Edge case when interval intersection is empty. For instance: {$and: [{"a": 1}, {"a" :
        // 5}]}. Create an ALWAYS_FALSE expression.
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    const auto path = StringData(oil->name);
    std::vector<std::unique_ptr<MatchExpression>> expressions;
    for (const auto& interval : oil->intervals) {
        auto expr = getMatchExpressionFromInterval(path, interval);
        if (!expr) {
            // We found an interval that cannot be transformed to match expression, bail out.
            return nullptr;
        }
        expressions.push_back(std::move(expr));
    }

    if (expressions.size() == 1) {
        return std::move(expressions[0]);
    }
    if (expressions.size() > 1) {
        // Make an OR match expression for the disjunction of intervals.
        return std::make_unique<OrMatchExpression>(std::move(expressions));
    }

    return nullptr;
}

}  // namespace

std::unique_ptr<MatchExpression> getMatchExpressionFromBounds(const IndexBounds& bounds,
                                                              const MatchExpression* filterExpr) {
    std::vector<std::unique_ptr<MatchExpression>> expressions;

    for (auto& oil : bounds.fields) {
        auto disj = getMatchExpressionFromOIL(&oil);
        if (!disj) {
            // We found an OIL with an interval that cannot be transformed to match expression, bail
            // out.
            return nullptr;
        }
        expressions.push_back(std::move(disj));
    }

    if (filterExpr) {
        expressions.push_back(filterExpr->clone());
    }

    if (expressions.size() == 0) {
        return nullptr;
    }

    auto expression = expressions.size() == 1
        ? std::move(expressions[0])
        : std::make_unique<AndMatchExpression>(std::move(expressions));

    // Normalize and simplify the resulting conjunction in order to enable better detection of
    // expression equivalence.
    auto simplified = normalizeMatchExpression(std::move(expression), true);
    if (simplified->isTriviallyTrue()) {
        // There are at least two different forms of true expressions. Return the same form.
        return std::make_unique<AlwaysTrueMatchExpression>();
    }
    return simplified;
}

}  // namespace mongo::cost_based_ranker
