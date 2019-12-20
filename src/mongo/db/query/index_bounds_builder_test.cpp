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

#include "mongo/db/query/index_bounds_builder_test.h"

#include <limits>
#include <memory>

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
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

//
// $elemMatch value
// Example: {a: {$elemMatch: {$gt: 2}}}
//

TEST_F(IndexBoundsBuilderTest, TranslateElemMatchValue) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteNumber) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteNumberMin) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteNegativeInfinity) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteObject) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteCode) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteCodeWScope) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteMinKey) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteMaxKey) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtNumber) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtNumberMin) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtNegativeInfinity) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtDate) {
    auto testIndex = buildSimpleIndexEntry();
    const auto date = Date_t::fromMillisSinceEpoch(5000);
    BSONObj obj = BSON("a" << LT << date);
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << Date_t::min() << "" << date), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtObject) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtCode) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtCodeWScope) {
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
TEST_F(IndexBoundsBuilderTest, TranslateLtMinKeyDoesNotGenerateBounds) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtMaxKey) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtTimestamp) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtNumber) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtNumberMax) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtPositiveInfinity) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtString) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtObject) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtCode) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtCodeWScope) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtMinKey) {
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
TEST_F(IndexBoundsBuilderTest, TranslateGtMaxKeyDoesNotGenerateBounds) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteNumber) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteNumberMax) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtePositiveInfinity) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteObject) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteCode) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteCodeWScope) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteMinKey) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteMaxKey) {
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

TEST_F(IndexBoundsBuilderTest, TranslateEqualNan) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtNan) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteNan) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtNan) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteNan) {
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

TEST_F(IndexBoundsBuilderTest, TranslateEqual) {
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

TEST_F(IndexBoundsBuilderTest, TranslateExprEqual) {
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

TEST_F(IndexBoundsBuilderTest, TranslateExprEqualToStringRespectsCollation) {
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

TEST_F(IndexBoundsBuilderTest, TranslateExprEqualHashedIndex) {
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

TEST_F(IndexBoundsBuilderTest, TranslateArrayEqualBasic) {
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

TEST_F(IndexBoundsBuilderTest, TranslateIn) {
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

TEST_F(IndexBoundsBuilderTest, TranslateInArray) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLteBinData) {
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

TEST_F(IndexBoundsBuilderTest, TranslateLtBinData) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGtBinData) {
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

TEST_F(IndexBoundsBuilderTest, TranslateGteBinData) {
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
// $exists tests
//

TEST_F(IndexBoundsBuilderTest, ExistsTrue) {
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

TEST_F(IndexBoundsBuilderTest, ExistsFalse) {
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

TEST_F(IndexBoundsBuilderTest, ExistsTrueSparse) {
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

TEST_F(IndexBoundsBuilderTest, UnionTwoLt) {
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

TEST_F(IndexBoundsBuilderTest, UnionDupEq) {
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

TEST_F(IndexBoundsBuilderTest, UnionGtLt) {
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

TEST_F(IndexBoundsBuilderTest, UnionTwoEmptyRanges) {
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

TEST_F(IndexBoundsBuilderTest, IntersectTwoLt) {
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

TEST_F(IndexBoundsBuilderTest, IntersectEqGte) {
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

TEST_F(IndexBoundsBuilderTest, IntersectGtLte) {
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

TEST_F(IndexBoundsBuilderTest, IntersectGtIn) {
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

TEST_F(IndexBoundsBuilderTest, IntersectionIsPointInterval) {
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

TEST_F(IndexBoundsBuilderTest, IntersectFullyContained) {
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

TEST_F(IndexBoundsBuilderTest, EmptyIntersection) {
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

TEST_F(IndexBoundsBuilderTest, TranslateMod) {
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
// Complementing bounds for negations
//

// Expected oil: [MinKey, 3), (3, MaxKey]
TEST_F(IndexBoundsBuilderTest, SimpleNE) {
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

TEST_F(IndexBoundsBuilderTest, IntersectWithNE) {
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

TEST_F(IndexBoundsBuilderTest, UnionizeWithNE) {
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

TEST_F(IndexBoundsBuilderTest, TypeArrayWithAdditionalTypesHasOpenBounds) {
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

TEST_F(IndexBoundsBuilderTest, TypeStringOrNumberHasCorrectBounds) {
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
                  oil.intervals[1].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, RedundantTypeNumberHasCorrectBounds) {
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
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, CanUseCoveredMatchingForEqualityPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$eq: 3}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_TRUE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForEqualityToArrayPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$eq: [1, 2, 3]}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForEqualityToNullPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: null}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForTypeArrayPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'array'}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForExistsTruePredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForExistsFalsePredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: false}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CanUseCoveredMatchingForExistsTrueWithSparseIndex) {
    auto testIndex = buildSimpleIndexEntry();
    testIndex.sparse = true;
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto expr = parseMatchExpression(obj);
    ASSERT_TRUE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, IntersectizeBasic) {
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
