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

#include "mongo/db/query/index_bounds_builder.h"

#include <limits>
#include <memory>

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/expression_index.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

using DoubleLimits = std::numeric_limits<double>;

double numberMin = -DoubleLimits::max();
double numberMax = DoubleLimits::max();
double negativeInfinity = -DoubleLimits::infinity();
double positiveInfinity = DoubleLimits::infinity();
double NaN = DoubleLimits::quiet_NaN();

/**
 * Make a minimal IndexEntry from just an optional key pattern. A dummy name will be added. An empty
 * key pattern will be used if none is provided.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp = BSONObj()) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            nullptr,
            {},
            nullptr,
            nullptr};
}

/**
 * Utility function to create MatchExpression
 */
std::unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
    ASSERT_TRUE(status.isOK());
    return std::unique_ptr<MatchExpression>(status.getValue().release());
}

/**
 * Given a list of queries in 'toUnion', translate into index bounds and return
 * the union of these bounds in the out-parameter 'oilOut'.
 */
void testTranslateAndUnion(const std::vector<BSONObj>& toUnion,
                           OrderedIntervalList* oilOut,
                           IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    auto testIndex = buildSimpleIndexEntry();

    for (auto it = toUnion.begin(); it != toUnion.end(); ++it) {
        auto expr = parseMatchExpression(*it);
        BSONElement elt = it->firstElement();
        if (toUnion.begin() == it) {
            IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
        } else {
            IndexBoundsBuilder::translateAndUnion(expr.get(), elt, testIndex, oilOut, tightnessOut);
        }
    }
}

/**
 * Given a list of queries in 'toUnion', translate into index bounds and return
 * the intersection of these bounds in the out-parameter 'oilOut'.
 */
void testTranslateAndIntersect(const std::vector<BSONObj>& toIntersect,
                               OrderedIntervalList* oilOut,
                               IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    auto testIndex = buildSimpleIndexEntry();

    for (auto it = toIntersect.begin(); it != toIntersect.end(); ++it) {
        auto expr = parseMatchExpression(*it);
        BSONElement elt = it->firstElement();
        if (toIntersect.begin() == it) {
            IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
        } else {
            IndexBoundsBuilder::translateAndIntersect(
                expr.get(), elt, testIndex, oilOut, tightnessOut);
        }
    }
}

/**
 * 'constraints' is a vector of BSONObj's representing match expressions, where
 * each filter is paired with a boolean. If the boolean is true, then the filter's
 * index bounds should be intersected with the other constraints; if false, then
 * they should be unioned. The resulting bounds are returned in the
 * out-parameter 'oilOut'.
 */
void testTranslate(const std::vector<std::pair<BSONObj, bool>>& constraints,
                   OrderedIntervalList* oilOut,
                   IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    auto testIndex = buildSimpleIndexEntry();

    for (auto it = constraints.begin(); it != constraints.end(); ++it) {
        BSONObj obj = it->first;
        bool isIntersect = it->second;
        auto expr = parseMatchExpression(obj);
        BSONElement elt = obj.firstElement();
        if (constraints.begin() == it) {
            IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
        } else if (isIntersect) {
            IndexBoundsBuilder::translateAndIntersect(
                expr.get(), elt, testIndex, oilOut, tightnessOut);
        } else {
            IndexBoundsBuilder::translateAndUnion(expr.get(), elt, testIndex, oilOut, tightnessOut);
        }
    }
}

/**
 * run isSingleInterval and return the result to calling test.
 */
bool testSingleInterval(IndexBounds bounds) {
    BSONObj startKey;
    bool startKeyIn;
    BSONObj endKey;
    bool endKeyIn;
    return IndexBoundsBuilder::isSingleInterval(bounds, &startKey, &startKeyIn, &endKey, &endKeyIn);
}

//
// $elemMatch value
// Example: {a: {$elemMatch: {$gt: 2}}}
//

TEST(IndexBoundsBuilderTest, TranslateElemMatchValue) {
    auto testIndex = buildSimpleIndexEntry();
    // Bounds generated should be the same as the embedded expression
    // except for the tightness.
    BSONObj obj = fromjson("{a: {$elemMatch: {$gt: 2}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 2, '': Infinity}"), false, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

//
// Comparison operators ($lte, $lt, $gt, $gte, $eq)
//

TEST(IndexBoundsBuilderTest, TranslateLteNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lte: 1}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteNumberMin) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << numberMin));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << negativeInfinity << "" << numberMin), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteNegativeInfinity) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lte: -Infinity}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': -Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteObject) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lte: {b: 1}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': {}, '': {b: 1}}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteCode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << BSONCode("function(){ return 0; }")));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[, function(){ return 0; }]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteCodeWScope) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(),
                  "[CodeWScope( , {}), CodeWScope( this.b == c, { c: 1 })]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteMinKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << MINKEY));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[MinKey, MinKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteMaxKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << MAXKEY));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[MinKey, MaxKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lt: 1}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtNumberMin) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << numberMin));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << negativeInfinity << "" << numberMin), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtNegativeInfinity) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lt: -Infinity}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtDate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << LT << Date_t::fromMillisSinceEpoch(5000));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(fromjson("{'': true, '': new Date(5000)}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtObject) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lt: {b: 1}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': {}, '': {b: 1}}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtCode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << BSONCode("function(){ return 0; }")));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[, function(){ return 0; })");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtCodeWScope) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(),
                  "[CodeWScope( , {}), CodeWScope( this.b == c, { c: 1 }))");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// Nothing can be less than MinKey so the resulting index bounds would be a useless empty range.
TEST(IndexBoundsBuilderTest, TranslateLtMinKeyDoesNotGenerateBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << MINKEY));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtMaxKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << MAXKEY));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[MinKey, MaxKey)");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtTimestamp) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << GT << Timestamp(2, 3));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  //  Constant below is ~0U, or 2**32 - 1, but cannot be written that way in JS
                  oil.intervals[0].compare(Interval(
                      fromjson("{'': Timestamp(2, 3), '': Timestamp(4294967295, 4294967295)}"),
                      false,
                      true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: 1}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 1, '': Infinity}"), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtNumberMax) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << numberMax));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << numberMax << "" << positiveInfinity), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtPositiveInfinity) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: Infinity}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtString) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: 'abc'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'abc', '': {}}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtObject) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: {b: 1}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': {b: 1}, '': []}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtCode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << BSONCode("function(){ return 0; }")));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "(function(){ return 0; }, CodeWScope( , {}))");
    ASSERT_FALSE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtCodeWScope) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "(CodeWScope( this.b == c, { c: 1 }), MaxKey)");
    ASSERT_FALSE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtMinKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << MINKEY));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "(MinKey, MaxKey]");
    ASSERT_FALSE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// Nothing can be greater than MaxKey so the resulting index bounds would be a useless empty range.
TEST(IndexBoundsBuilderTest, TranslateGtMaxKeyDoesNotGenerateBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << MAXKEY));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gte: 1}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 1, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteNumberMax) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << numberMax));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << numberMax << "" << positiveInfinity), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtePositiveInfinity) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gte: Infinity}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': Infinity, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteObject) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gte: {b: 1}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': {b: 1}, '': []}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteCode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << BSONCode("function(){ return 0; }")));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[function(){ return 0; }, CodeWScope( , {}))");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteCodeWScope) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[CodeWScope( this.b == c, { c: 1 }), MaxKey)");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteMinKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << MINKEY));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[MinKey, MaxKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteMaxKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << MAXKEY));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(), "[MaxKey, MaxKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateEqualNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: NaN}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lt: NaN}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lte: NaN}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: NaN}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gte: NaN}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateEqual) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << 4);
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 4, '': 4}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateExprEqual) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprEq" << 4));
    auto expr = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 4, '': 4}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateExprEqualToStringRespectsCollation) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << BSON("$_internalExprEq"
                                   << "foo"));
    auto expr = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateExprEqualHashedIndex) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprEq" << 4));
    auto expr = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    BSONObj expectedHash = ExpressionMapping::hash(BSON("" << 4).firstElement());
    BSONObjBuilder intervalBuilder;
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    BSONObj intervalObj = intervalBuilder.obj();

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(intervalObj, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateExprEqualToNullIsInexactFetch) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprEq" << BSONNULL));
    auto expr = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': undefined, '': undefined}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateArrayEqualBasic) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: [1, 2, 3]}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': [1, 2, 3], '': [1, 2, 3]}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateIn) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$in: [8, 44, -1, -3]}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 4U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': -3, '': -3}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': -1, '': -1}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[2].compare(Interval(fromjson("{'': 8, '': 8}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[3].compare(Interval(fromjson("{'': 44, '': 44}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateInArray) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$in: [[1], 2]}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 3U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[2].compare(Interval(fromjson("{'': [1], '': [1]}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateLteBinData) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson(
        "{a: {$lte: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA',"
        "$type: '00'}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '', $type: '00'},"
                                    "'': {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}"),
                           true,
                           true)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtBinData) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson(
        "{a: {$lt: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA',"
        "$type: '00'}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '', $type: '00'},"
                                    "'': {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}"),
                           true,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtBinData) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson(
        "{a: {$gt: {$binary: '////////////////////////////',"
        "$type: '00'}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '////////////////////////////', $type: '00'},"
                                    "'': ObjectId('000000000000000000000000')}"),
                           false,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteBinData) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson(
        "{a: {$gte: {$binary: '////////////////////////////',"
        "$type: '00'}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '////////////////////////////', $type: '00'},"
                                    "'': ObjectId('000000000000000000000000')}"),
                           true,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
}

//
// $type
//

TEST(IndexBoundsBuilderTest, TypeNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'number'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    // Build the expected interval.
    BSONObjBuilder bob;
    BSONType type = BSONType::NumberInt;
    bob.appendMinForType("", type);
    bob.appendMaxForType("", type);
    BSONObj expectedInterval = bob.obj();

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

//
// $exists tests
//

TEST(IndexBoundsBuilderTest, ExistsTrue) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, ExistsFalse) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: false}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, ExistsTrueSparse) {
    auto keyPattern = BSONObj();
    IndexEntry testIndex =
        IndexEntry(keyPattern,
                   IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                   false,  // multikey
                   {},
                   {},
                   true,   // sparse
                   false,  // unique
                   IndexEntry::Identifier{"exists_true_sparse"},
                   nullptr,  // filterExpr
                   BSONObj(),
                   nullptr,
                   nullptr);
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

//
// Union tests
//

TEST(IndexBoundsBuilderTest, UnionTwoLt) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toUnion;
    toUnion.push_back(fromjson("{a: {$lt: 1}}"));
    toUnion.push_back(fromjson("{a: {$lt: 5}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndUnion(toUnion, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 5}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, UnionDupEq) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toUnion;
    toUnion.push_back(fromjson("{a: 1}"));
    toUnion.push_back(fromjson("{a: 5}"));
    toUnion.push_back(fromjson("{a: 1}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndUnion(toUnion, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': 5, '': 5}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, UnionGtLt) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toUnion;
    toUnion.push_back(fromjson("{a: {$gt: 1}}"));
    toUnion.push_back(fromjson("{a: {$lt: 3}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndUnion(toUnion, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, UnionTwoEmptyRanges) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<std::pair<BSONObj, bool>> constraints;
    constraints.push_back(std::make_pair(fromjson("{a: {$gt: 1}}"), true));
    constraints.push_back(std::make_pair(fromjson("{a: {$lte: 0}}"), true));
    constraints.push_back(std::make_pair(fromjson("{a: {$in:[]}}"), false));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslate(constraints, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
}

//
// Intersection tests
//

TEST(IndexBoundsBuilderTest, IntersectTwoLt) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: {$lt: 1}}"));
    toIntersect.push_back(fromjson("{a: {$lt: 5}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndIntersect(toIntersect, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, IntersectEqGte) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: 1}}"));
    toIntersect.push_back(fromjson("{a: {$gte: 1}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndIntersect(toIntersect, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, IntersectGtLte) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: {$gt: 0}}"));
    toIntersect.push_back(fromjson("{a: {$lte: 10}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndIntersect(toIntersect, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 0, '': 10}"), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, IntersectGtIn) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: {$gt: 4}}"));
    toIntersect.push_back(fromjson("{a: {$in: [1,2,3,4,5,6]}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndIntersect(toIntersect, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 5, '': 5}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': 6, '': 6}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, IntersectionIsPointInterval) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: {$gte: 1}}"));
    toIntersect.push_back(fromjson("{a: {$lte: 1}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndIntersect(toIntersect, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, IntersectFullyContained) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: {$gt: 5}}"));
    toIntersect.push_back(fromjson("{a: {$lt: 15}}"));
    toIntersect.push_back(fromjson("{a: {$gte: 6}}"));
    toIntersect.push_back(fromjson("{a: {$lte: 13}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndIntersect(toIntersect, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 6, '': 13}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, EmptyIntersection) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: 1}}"));
    toIntersect.push_back(fromjson("{a: {$gte: 2}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndIntersect(toIntersect, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
}

//
// $mod
//

TEST(IndexBoundsBuilderTest, TranslateMod) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$mod: [2, 0]}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << NaN << "" << positiveInfinity), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

//
// Test simpleRegex
//

TEST(SimpleRegexTest, RootedLine) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^foo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedString) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedOptionalFirstChar) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^f?oo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedOptionalSecondChar) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^fz?oo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "f");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedMultiline) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^foo", "m", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedStringMultiline) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "m", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedCaseInsensitiveMulti) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "mi", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedComplex) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(
        "\\Af \t\vo\n\ro  \\ \\# #comment", "mx", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo #");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedLiteral) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^\\Qasdf\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralWithExtra) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qasdf\\E.*", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedLiteralNoEnd) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^\\Qasdf", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralBackslash) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qasdf\\\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf\\");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralDotStar) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qas.*df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as.*df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralNestedEscape) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qas\\Q[df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as\\Q[df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralNestedEscapeEnd) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qas\\E\\\\E\\Q$df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as\\E$df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// An anchored regular expression that uses the "|" operator is not considered "simple" and has
// non-tight index bounds.
TEST(SimpleRegexTest, PipeCharacterUsesInexactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^(a(a|$)|b", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, PipeCharacterUsesInexactBoundsWithTwoPrefixes) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^(a(a|$)|^b", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, PipeCharacterPrecededByEscapedBackslashUsesInexactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(R"(^a\\|b)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);

    prefix = IndexBoundsBuilder::simpleRegex(R"(^(foo\\|bar)\\|baz)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

// However, a regular expression with an escaped pipe (that is, using no special meaning) can use
// exact index bounds.
TEST(SimpleRegexTest, PipeCharacterEscapedWithBackslashUsesExactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(R"(^a\|b)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "a|b");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);

    prefix = IndexBoundsBuilder::simpleRegex(R"(^\|1\|2\|\|)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "|1|2||");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, FalsePositiveOnPipeInQEEscapeSequenceUsesInexactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(R"(^\Q|\E)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, FalsePositiveOnPipeInCharacterClassUsesInexactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(R"(^[|])", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

// SERVER-9035
TEST(SimpleRegexTest, RootedSingleLineMode) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^foo", "s", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// SERVER-9035
TEST(SimpleRegexTest, NonRootedSingleLineMode) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("foo", "s", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

// SERVER-9035
TEST(SimpleRegexTest, RootedComplexSingleLineMode) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(
        "\\Af \t\vo\n\ro  \\ \\# #comment", "msx", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo #");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedRegexCantBeIndexedTightlyIfIndexHasCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^foo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

//
// Regex bounds
//

TEST(IndexBoundsBuilderTest, SimpleNonPrefixRegex) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: /foo/}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /foo/, '': /foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(IndexBoundsBuilderTest, NonSimpleRegexWithPipe) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: /^foo.*|bar/}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(
                      Interval(fromjson("{'': /^foo.*|bar/, '': /^foo.*|bar/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(IndexBoundsBuilderTest, SimpleRegexSingleLineMode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: /^foo/s}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'foo', '': 'fop'}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/s, '': /^foo/s}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, SimplePrefixRegex) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: /^foo/}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'foo', '': 'fop'}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

//
// isSingleInterval
//

TEST(IndexBoundsBuilderTest, SingleFieldEqualityInterval) {
    // Equality on a single field is a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    bounds.fields.push_back(oil);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, SingleIntervalSingleFieldInterval) {
    // Single interval on a single field is a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(fromjson("{ '':5, '':Infinity }"), true, true));
    bounds.fields.push_back(oil);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, MultipleIntervalsSingleFieldInterval) {
    // Multiple intervals on a single field is not a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(fromjson("{ '':4, '':5 }"), true, true));
    oil.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    bounds.fields.push_back(oil);
    ASSERT(!testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualityTwoFieldsInterval) {
    // Equality on two fields is a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(BSON("" << 6 << "" << 6), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualityFirstFieldSingleIntervalSecondFieldInterval) {
    // Equality on first field and single interval on second field
    // is a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':6, '':Infinity }"), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, SingleIntervalFirstAndSecondFieldsInterval) {
    // Single interval on first field and single interval on second field is
    // not a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(fromjson("{ '':-Infinity, '':5 }"), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':6, '':Infinity }"), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(!testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, MultipleIntervalsTwoFieldsInterval) {
    // Multiple intervals on two fields is not a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 4 << "" << 4), true, true));
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(BSON("" << 7 << "" << 7), true, true));
    oil_b.intervals.push_back(Interval(BSON("" << 8 << "" << 8), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(!testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, MissingSecondFieldInterval) {
    // when second field is not specified, still a compound single interval
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(IndexBoundsBuilder::allValues());
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualityTwoFieldsIntervalThirdInterval) {
    // Equality on first two fields and single interval on third is a
    // compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(BSON("" << 6 << "" << 6), true, true));
    oil_c.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualitySingleIntervalMissingInterval) {
    // Equality, then Single Interval, then missing is a compound single interval
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    oil_c.intervals.push_back(IndexBoundsBuilder::allValues());
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualitySingleMissingMissingInterval) {
    // Equality, then single interval, then missing, then missing,
    // is a compound single interval
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    OrderedIntervalList oil_d("d");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    oil_c.intervals.push_back(IndexBoundsBuilder::allValues());
    oil_d.intervals.push_back(IndexBoundsBuilder::allValues());
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    bounds.fields.push_back(oil_d);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualitySingleMissingMissingMixedInterval) {
    // Equality, then single interval, then missing, then missing, with mixed order
    // fields is a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    OrderedIntervalList oil_d("d");
    IndexBounds bounds;
    Interval allValues = IndexBoundsBuilder::allValues();
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    oil_c.intervals.push_back(allValues);
    IndexBoundsBuilder::reverseInterval(&allValues);
    oil_d.intervals.push_back(allValues);
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    bounds.fields.push_back(oil_d);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualitySingleMissingSingleInterval) {
    // Equality, then single interval, then missing, then single interval is not
    // a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    OrderedIntervalList oil_d("d");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    oil_c.intervals.push_back(IndexBoundsBuilder::allValues());
    oil_d.intervals.push_back(Interval(fromjson("{ '':1, '':Infinity }"), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    bounds.fields.push_back(oil_d);
    ASSERT(!testSingleInterval(bounds));
}

//
// Complementing bounds for negations
//

/**
 * Get a BSONObj which represents the interval from
 * MinKey to 'end'.
 */
BSONObj minKeyIntObj(int end) {
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendNumber("", end);
    return bob.obj();
}

/**
 * Get a BSONObj which represents the interval from
 * 'start' to MaxKey.
 */
BSONObj maxKeyIntObj(int start) {
    BSONObjBuilder bob;
    bob.appendNumber("", start);
    bob.appendMaxKey("");
    return bob.obj();
}

// Expected oil: [MinKey, 3), (3, MaxKey]
TEST(IndexBoundsBuilderTest, SimpleNE) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$ne" << 3));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(3), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(maxKeyIntObj(3), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, IntersectWithNE) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: {$gt: 1}}"));
    toIntersect.push_back(fromjson("{a: {$ne: 2}}}"));
    toIntersect.push_back(fromjson("{a: {$lte: 6}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndIntersect(toIntersect, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(BSON("" << 1 << "" << 2), false, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(BSON("" << 2 << "" << 6), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, UnionizeWithNE) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toUnionize;
    toUnionize.push_back(fromjson("{a: {$ne: 3}}"));
    toUnionize.push_back(fromjson("{a: {$ne: 4}}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    testTranslateAndUnion(toUnionize, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// Test $type bounds for Code BSON type.
TEST(IndexBoundsBuilderTest, CodeTypeBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 13}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Build the expected interval.
    BSONObjBuilder bob;
    bob.appendCode("", "");
    bob.appendCodeWScope("", "", BSONObj());
    BSONObj expectedInterval = bob.obj();

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

// Test $type bounds for Code With Scoped BSON type.
TEST(IndexBoundsBuilderTest, CodeWithScopeTypeBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 15}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Build the expected interval.
    BSONObjBuilder bob;
    bob.appendCodeWScope("", "", BSONObj());
    bob.appendMaxKey("");
    BSONObj expectedInterval = bob.obj();

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

// Test $type bounds for double BSON type.
TEST(IndexBoundsBuilderTest, DoubleTypeBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 1}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Build the expected interval.
    BSONObjBuilder bob;
    bob.appendNumber("", NaN);
    bob.appendNumber("", positiveInfinity);
    BSONObj expectedInterval = bob.obj();

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TypeArrayBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'array'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

//
// Collation-related tests.
//

TEST(IndexBoundsBuilderTest, TranslateEqualityToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = BSON("a"
                       << "foo");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateEqualityToNonStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << 3);
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 3, '': 3}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

/**
 * Asserts that 'oil' contains exactly two bounds: [[undefined, undefined], [null, null]].
 */
void assertBoundsRepresentEqualsNull(const OrderedIntervalList& oil) {
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': undefined, '': undefined}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
}

TEST(IndexBoundsBuilderTest, TranslateEqualsToNullShouldBuildInexactBounds) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    BSONObj obj = BSON("a" << BSONNULL);
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest, TranslateDottedEqualsToNullShouldBuildInexactBounds) {
    BSONObj indexPattern = BSON("a.b" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    BSONObj obj = BSON("a.b" << BSONNULL);
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a.b");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest, TranslateEqualsToNullMultiKeyShouldBuildInexactBounds) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    BSONObj obj = BSON("a" << BSONNULL);
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest, TranslateEqualsToNullShouldBuildTwoIntervalsForHashedIndex) {
    BSONObj indexPattern = BSON("a"
                                << "hashed");
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.type = IndexType::INDEX_HASHED;

    BSONObj obj = BSON("a" << BSONNULL);
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    // We should have one for undefined, and one for null.
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    {
        const BSONObj undefinedElementObj = BSON("" << BSONUndefined);
        const BSONObj hashedUndefinedInterval =
            ExpressionMapping::hash(undefinedElementObj.firstElement());
        ASSERT_EQ(hashedUndefinedInterval.firstElement().type(), BSONType::NumberLong);

        const auto& firstInterval = oil.intervals[0];
        ASSERT_TRUE(firstInterval.startInclusive);
        ASSERT_TRUE(firstInterval.endInclusive);
        ASSERT_EQ(firstInterval.start.type(), BSONType::NumberLong);
        ASSERT_EQ(firstInterval.start.numberLong(),
                  hashedUndefinedInterval.firstElement().numberLong());
    }

    {
        const BSONObj nullElementObj = BSON("" << BSONNULL);
        const BSONObj hashedNullInterval = ExpressionMapping::hash(nullElementObj.firstElement());
        ASSERT_EQ(hashedNullInterval.firstElement().type(), BSONType::NumberLong);

        const auto& secondInterval = oil.intervals[1];
        ASSERT_TRUE(secondInterval.startInclusive);
        ASSERT_TRUE(secondInterval.endInclusive);
        ASSERT_EQ(secondInterval.start.type(), BSONType::NumberLong);
        ASSERT_EQ(secondInterval.start.numberLong(),
                  hashedNullInterval.firstElement().numberLong());
    }
}

/**
 * Asserts that 'oil' contains exactly two bounds: [MinKey, undefined) and (null, MaxKey].
 */
void assertBoundsRepresentNotEqualsNull(const OrderedIntervalList& oil) {
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    {
        BSONObjBuilder bob;
        bob.appendMinKey("");
        bob.appendUndefined("");
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[0].compare(Interval(bob.obj(), true, false)));
    }

    {
        BSONObjBuilder bob;
        bob.appendNull("");
        bob.appendMaxKey("");
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[1].compare(Interval(bob.obj(), false, true)));
    }
}

TEST(IndexBoundsBuilderTest, TranslateNotEqualToNullShouldBuildExactBoundsIfIndexIsNotMultiKey) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    BSONObj obj = BSON("a" << BSON("$ne" << BSONNULL));
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    // Bounds should be [MinKey, undefined), (null, MaxKey].
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertBoundsRepresentNotEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest,
     TranslateNotEqualToNullShouldBuildExactBoundsIfIndexIsNotMultiKeyOnRelevantPath) {
    BSONObj indexPattern = BSON("a" << 1 << "b" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikeyPaths = {{}, {0}};  // "a" is not multi-key, but "b" is.

    BSONObj obj = BSON("a" << BSON("$ne" << BSONNULL));
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    // Bounds should be [MinKey, undefined), (null, MaxKey].
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertBoundsRepresentNotEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest, TranslateNotEqualToNullShouldBuildExactBoundsOnReverseIndex) {
    BSONObj indexPattern = BSON("a" << -1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    BSONObj obj = BSON("a" << BSON("$ne" << BSONNULL));
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    // Bounds should be [MinKey, undefined), (null, MaxKey].
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertBoundsRepresentNotEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest, TranslateNotEqualToNullShouldBuildInexactBoundsIfIndexIsMultiKey) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    BSONObj matchObj = BSON("a" << BSON("$ne" << BSONNULL));
    auto expr = parseMatchExpression(matchObj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest,
     TranslateDottedElemMatchValueNotEqualToNullShouldBuildExactBoundsIfIsMultiKeyOnThatPath) {
    BSONObj indexPattern = BSON("a.b" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikeyPaths = {{1}};  // "a.b" is multikey.

    BSONObj matchObj = BSON("a.b" << BSON("$elemMatch" << BSON("$ne" << BSONNULL)));
    auto expr = parseMatchExpression(matchObj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a.b");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest,
     TranslateDottedFieldNotEqualToNullShouldBuildInexactBoundsIfIndexIsMultiKey) {
    BSONObj indexPattern = BSON("a.b" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    BSONObj matchObj = BSON("a.b" << BSON("$ne" << BSONNULL));
    auto expr = parseMatchExpression(matchObj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a.b");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest,
     TranslateElemMatchValueNotEqualToNullShouldBuildInexactBoundsIfIndexIsMultiKey) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    BSONObj obj = BSON("a" << BSON("$elemMatch" << BSON("$ne" << BSONNULL)));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest,
     TranslateElemMatchValueNotEqualToNullShouldBuildInExactBoundsIfIndexIsNotMultiKey) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    BSONObj matchObj = BSON("a" << BSON("$elemMatch" << BSON("$ne" << BSONNULL)));
    auto expr = parseMatchExpression(matchObj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
}

TEST(IndexBoundsBuilderTest, TranslateNotEqualToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << BSON("$ne"
                                   << "bar"));
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Bounds should be [MinKey, "rab"), ("rab", MaxKey].
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);

    {
        BSONObjBuilder bob;
        bob.appendMinKey("");
        bob.append("", "rab");
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[0].compare(Interval(bob.obj(), true, false)));
    }

    {
        BSONObjBuilder bob;
        bob.append("", "rab");
        bob.appendMaxKey("");
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[1].compare(Interval(bob.obj(), false, true)));
    }
}

TEST(IndexBoundsBuilderTest, TranslateEqualToStringElemMatchValueWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$elemMatch: {$eq: 'baz'}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'zab', '': 'zab'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateLTEToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: 'foo'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLTEToNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: 3}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 3}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLTStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: 'foo'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': 'oof'}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLTNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: 3}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 3}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGTStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: 'foo'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': {}}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGTNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: 3}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 3, '': Infinity}"), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGTEToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: 'foo'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': {}}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGTEToNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: 3}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 3, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, SimplePrefixRegexWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: /^foo/}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, NotWithMockCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$ne:  3}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(3), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(maxKeyIntObj(3), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, ExistsTrueWithMockCollatorAndSparseIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;
    testIndex.sparse = true;

    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, ExistsFalseWithMockCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$exists: false}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TypeStringIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$type: 'string'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, InWithStringAndCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: ['foo']}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, InWithNumberAndStringAndCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [2, 'foo']}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, InWithRegexAndCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [/^foo/]}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, InWithNumberAndCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [2]}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, LTEMaxKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: {$maxKey: 1}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, LTMaxKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: {$maxKey: 1}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValuesRespectingInclusion(
                      BoundInclusion::kIncludeStartKeyOnly)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, GTEMinKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: {$minKey: 1}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, GTMinKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: {$minKey: 1}}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValuesRespectingInclusion(
                      BoundInclusion::kIncludeEndKeyOnly)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, StringEqualityAgainstHashedIndexWithCollatorUsesHashOfCollationKey) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: 'foo'}");
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    BSONObj expectedCollationKey = BSON(""
                                        << "oof");
    BSONObj expectedHash = ExpressionMapping::hash(expectedCollationKey.firstElement());
    BSONObjBuilder intervalBuilder;
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    BSONObj intervalObj = intervalBuilder.obj();

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(intervalObj, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, EqualityToNumberAgainstHashedIndexWithCollatorUsesHash) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: 3}");
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    BSONObj expectedHash = ExpressionMapping::hash(obj.firstElement());
    BSONObjBuilder intervalBuilder;
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    BSONObj intervalObj = intervalBuilder.obj();

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(intervalObj, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, InWithStringAgainstHashedIndexWithCollatorUsesHashOfCollationKey) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: ['foo']}}");
    auto expr = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    BSONObj expectedCollationKey = BSON(""
                                        << "oof");
    BSONObj expectedHash = ExpressionMapping::hash(expectedCollationKey.firstElement());
    BSONObjBuilder intervalBuilder;
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    BSONObj intervalObj = intervalBuilder.obj();

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(intervalObj, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TypeArrayWithAdditionalTypesHasOpenBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: ['array', 'long']}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TypeStringOrNumberHasCorrectBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: ['string', 'number']}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': Infinity}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': '', '': {}}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, RedundantTypeNumberHasCorrectBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: ['number', 'int', 'long', 'double']}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': Infinity}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, CanUseCoveredMatchingForEqualityPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$eq: 3}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_TRUE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST(IndexBoundsBuilderTest, CannotUseCoveredMatchingForEqualityToArrayPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$eq: [1, 2, 3]}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST(IndexBoundsBuilderTest, CannotUseCoveredMatchingForEqualityToNullPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: null}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST(IndexBoundsBuilderTest, CannotUseCoveredMatchingForTypeArrayPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'array'}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST(IndexBoundsBuilderTest, CannotUseCoveredMatchingForExistsTruePredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST(IndexBoundsBuilderTest, CannotUseCoveredMatchingForExistsFalsePredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: false}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST(IndexBoundsBuilderTest, CanUseCoveredMatchingForExistsTrueWithSparseIndex) {
    auto testIndex = buildSimpleIndexEntry();
    testIndex.sparse = true;
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_TRUE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST(IndexBoundsBuilderTest, IntersectizeBasic) {
    OrderedIntervalList oil1("xyz");
    oil1.intervals = {Interval(BSON("" << 0 << "" << 5), false, false)};

    OrderedIntervalList oil2("xyz");
    oil2.intervals = {Interval(BSON("" << 1 << "" << 6), false, false)};

    IndexBoundsBuilder::intersectize(oil1, &oil2);

    OrderedIntervalList expectedIntersection("xyz");
    expectedIntersection.intervals = {Interval(BSON("" << 1 << "" << 5), false, false)};

    ASSERT_TRUE(oil2 == expectedIntersection);
}

}  // namespace
