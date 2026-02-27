/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/unordered_fields_bsonelement_comparator.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <vector>

#include <fmt/format.h>

MONGO_MOD_PUBLIC;

namespace mongo::unittest::match {
using namespace ::testing;
using ::testing::Matcher;

// TODO(SERVER-120602): Replace when we have a safer bin data API.
struct BSONBinDataView {
    BinDataType type;
    StringData data;

    friend bool operator==(const BSONBinDataView&, const BSONBinDataView&) = default;
};

namespace MONGO_MOD_FILE_PRIVATE match_details {

using AnythingMatcher = decltype(_);

template <typename M>
bool typeTolerantMatch(const Matcher<M>& matcher,
                       const M& value,
                       MatchResultListener* resultListener) {
    return ExplainMatchResult(matcher, value, resultListener);
}

template <typename M, typename T>
bool typeTolerantMatch(const Matcher<M>& matcher,
                       const T& value,
                       MatchResultListener* resultListener) {
    return false;
}

template <typename T>
bool typeTolerantMatch(const AnythingMatcher& matcher,
                       const T& value,
                       MatchResultListener* resultListener) {
    return true;
}

template <typename T>
std::string describeTypeErasedMatcher(const Matcher<T>& matcher, bool negation = false) {
    return DescribeMatcher<T>(matcher, negation);
}

inline std::string describeTypeErasedMatcher(const AnythingMatcher& matcher,
                                             bool negation = false) {
    return negation ? "never matches" : "is anything";
}

struct NoRepresentation {
    constexpr explicit NoRepresentation() = default;
};

inline BSONBinDataView makeBinDataView(BSONElement el) {
    int size;
    auto data = el.binData(size);
    return {
        .type = el.binDataType(),
        .data = StringData{data, static_cast<size_t>(size)},
    };
}

}  // namespace MONGO_MOD_FILE_PRIVATE match_details

/**
 * `BSONObjElements(match)` match all elements of a BSONObj against a matcher.
 *
 * Example:
 *  ASSERT_THAT(o, BSONObjElements(Contains(BSONElementEQ(element))));
 */
MATCHER_P(BSONObjElements,
          match,
          fmt::format("is a BSONObj whose elements {}",
                      DescribeMatcher<std::vector<BSONElement>>(match, negation))) {
    std::vector<BSONElement> elems;
    arg.elems(elems);
    return ExplainMatchResult(match, elems, result_listener);
}

/**
 * `IsBSONElement(name, type, value)` matches a BSON element against matchers for its name, type and
 * value.
 *
 * The value matcher must be a Matcher<T>
 *
 * Example:
 *  ASSERT_THAT(el, IsBSONElement("el", BSONType::number, Matcher<int>(100)));
 */
MATCHER_P3(
    IsBSONElement,
    name,
    type,
    value,
    fmt::format("is a BSONElement which {}has name which {} and type which {} and value which {}{}",
                negation ? "not (" : "",
                DescribeMatcher<StringData>(name),
                DescribeMatcher<BSONType>(type),
                match_details::describeTypeErasedMatcher(value),
                negation ? ")" : "")) {
    bool nameAndTypeMatchResult =
        ExplainMatchResult(AllOf(Property("name", &BSONElement::fieldNameStringData, name),
                                 Property("type", &BSONElement::type, type)),
                           arg,
                           result_listener);

    if (!nameAndTypeMatchResult)
        return false;

    auto matchAs = [&](auto&& v) {
        return match_details::typeTolerantMatch(value, v, result_listener);
    };
    switch (arg.type()) {
        case BSONType::numberInt:
            return matchAs(arg.Int());
        case BSONType::numberLong:
            return matchAs(arg.Long());
        case BSONType::numberDouble:
            return matchAs(arg.Double());
        case BSONType::string:
            return matchAs(arg.String());
        case BSONType::date:
            return matchAs(arg.Date());
        case BSONType::boolean:
            return matchAs(arg.Bool());
        case BSONType::numberDecimal:
            return matchAs(arg.Decimal());
        case BSONType::oid:
            return matchAs(arg.OID());
        case BSONType::object:
            return matchAs(arg.Obj());
        case BSONType::array:
            return matchAs(arg.Array());
        case BSONType::timestamp:
            return matchAs(arg.timestamp());
        case BSONType::binData:
            return matchAs(match_details::makeBinDataView(arg));
        case BSONType::minKey:
        case BSONType::eoo:
        case BSONType::undefined:
        case BSONType::null:
        case BSONType::regEx:
        case BSONType::dbRef:
        case BSONType::code:
        case BSONType::symbol:
        case BSONType::codeWScope:
        // We cannot handle jsTypeMax as it has the same value as numberDecimal
        // case BSONType::jsTypeMax:
        case BSONType::maxKey:
            return matchAs(match_details::NoRepresentation{});
    }

    MONGO_UNREACHABLE;
}

#define BSON_MATCHER_CMP_(objType, name, comparator, operatorName, operatorExpr)                   \
    MATCHER_P(                                                                                     \
        name,                                                                                      \
        rhs,                                                                                       \
        fmt::format(                                                                               \
            "{} a {} {} {}", negation ? "isn't" : "is", #objType, operatorName, rhs.toString())) { \
        return comparator.evaluate(arg operatorExpr rhs);                                          \
    }

/**
 * `BSONObj{EQ,NE,LT,LE,GT,GE}(obj)` Comparsion machers for bson objects that uses the
 * SimpleBSONObjComparator.
 *
 * Examples:
 *   ASSERT_THAT(bsonObj, BSONObjEQ(expectedObj));
 *   ASSERT_THAT(bsonObj, BSONObjLT(greaterObj));
 *   ASSERT_THAT(bsonObj, BSONObjNE(differentObj));
 */
#define BSON_MATCHER_SIMPLE_CMP_(name, desc, op) \
    BSON_MATCHER_CMP_(BSONObj, name, SimpleBSONObjComparator{}, desc, op)
BSON_MATCHER_SIMPLE_CMP_(BSONObjEQ, "equal to", ==);
BSON_MATCHER_SIMPLE_CMP_(BSONObjNE, "not equal to", !=);
BSON_MATCHER_SIMPLE_CMP_(BSONObjLT, "<", <);
BSON_MATCHER_SIMPLE_CMP_(BSONObjLE, "<=", <=);
BSON_MATCHER_SIMPLE_CMP_(BSONObjGT, ">", >);
BSON_MATCHER_SIMPLE_CMP_(BSONObjGE, ">=", >=);
#undef BSON_MATCHER_SIMPLE_CMP_

/**
 * `BSONElement{EQ,NE,LT,LE,GT,GE}(elem)` Comparsion machers for bson elemens that uses
 * the SimpleBSONElementComparator.
 *
 * Examples:
 *   ASSERT_THAT(bsonObj["el1"], BSONElementEQ(expectedElement));
 *   ASSERT_THAT(bsonObj["el2"], BSONElementLT(greaterElement));
 *   ASSERT_THAT(bsonObj["el3"], BSONElementNE(differentElement));
 */
#define BSON_MATCHER_ELCMP_(name, desc, op) \
    BSON_MATCHER_CMP_(BSONElement, name, SimpleBSONElementComparator{}, desc, op)
BSON_MATCHER_ELCMP_(BSONElementEQ, "equal to", ==);
BSON_MATCHER_ELCMP_(BSONElementNE, "not equal to", !=);
BSON_MATCHER_ELCMP_(BSONElementLT, "<", <);
BSON_MATCHER_ELCMP_(BSONElementLE, "<=", <=);
BSON_MATCHER_ELCMP_(BSONElementGT, ">", >);
BSON_MATCHER_ELCMP_(BSONElementGE, ">=", >=);
#undef BSON_MATCHER_ELCMP_

/**
 * `BSONObjUnodered{EQ,NE,LT,LE,GT,GE}(obj)` Comparsion machers for bson objects that uses the
 * UnorderedFieldsBSONObjComparator for unordered comparison.
 *
 * Examples:
 *   ASSERT_THAT(bsonObj, BSONObjUnorderedEQ(expectedObj));
 *   ASSERT_THAT(bsonObj, BSONObjUnorderedLT(greaterObj));
 *   ASSERT_THAT(bsonObj, BSONObjUnorderedNE(differentObj));
 */
#define BSON_MATCHER_OBJ_UCMP_(name, desc, op) \
    BSON_MATCHER_CMP_(BSONObj, name, UnorderedFieldsBSONObjComparator{}, desc, op);
BSON_MATCHER_OBJ_UCMP_(BSONObjUnorderedEQ, "unordered equal to", ==);
BSON_MATCHER_OBJ_UCMP_(BSONObjUnorderedNE, "unordered not equal to", !=);
BSON_MATCHER_OBJ_UCMP_(BSONObjUnorderedLT, "unordered <", <);
BSON_MATCHER_OBJ_UCMP_(BSONObjUnorderedLE, "unordered <=", <=);
BSON_MATCHER_OBJ_UCMP_(BSONObjUnorderedGT, "unordered >", >);
BSON_MATCHER_OBJ_UCMP_(BSONObjUnorderedGE, "unordered >=", >=);
#undef BSON_MATCHER_OBJ_UCMP_

/**
 * `BSONElementUnordered{EQ,NE,LT,LE,GT,GE}(elem)` Comparsion machers for bson elemens that uses
 * the UnorderedFieldsBSONElementComparator for unordered comparison.
 *
 * Examples:
 *   ASSERT_THAT(bsonObj["el1"], BSONElementUnorderedEQ(expectedElement));
 *   ASSERT_THAT(bsonObj["el2"], BSONElementUnorderedLT(greaterElement));
 *   ASSERT_THAT(bsonObj["el3"], BSONElementUnorderedNE(differentElement));
 */
#define BSON_MATCHER_EL_UCMP_(name, desc, op) \
    BSON_MATCHER_CMP_(BSONElement, name, UnorderedFieldsBSONElementComparator{}, desc, op)
BSON_MATCHER_EL_UCMP_(BSONElementUnorderedEQ, "unordered equal to", ==);
BSON_MATCHER_EL_UCMP_(BSONElementUnorderedNE, "unordered not equal to", !=);
BSON_MATCHER_EL_UCMP_(BSONElementUnorderedLT, "unordered <", <);
BSON_MATCHER_EL_UCMP_(BSONElementUnorderedLE, "unordered <=", <=);
BSON_MATCHER_EL_UCMP_(BSONElementUnorderedGT, "unordered >", >);
BSON_MATCHER_EL_UCMP_(BSONElementUnorderedGE, "unordered >=", >=);
#undef BSON_MATCHER_EL_UCMP_

#undef BSON_MATCHER_CMP_

}  // namespace mongo::unittest::match
