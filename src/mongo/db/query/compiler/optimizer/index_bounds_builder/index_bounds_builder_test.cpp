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

#include <s2cellid.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/expression_geo_index_mapping.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder_test_fixture.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/time_support.h"

#include <limits>
#include <memory>

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
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 2, '': Infinity}"), false, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

//
// Comparison operators ($lte, $lt, $gt, $gte, $eq)
//

TEST_F(IndexBoundsBuilderTest, TranslateLteNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lte: 1}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteNumberMin) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << numberMin));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << negativeInfinity << "" << numberMin), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteNegativeInfinity) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lte: -Infinity}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': -Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteObject) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lte: {b: 1}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': {}, '': {b: 1}}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteCode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << BSONCode("function(){ return 0; }")));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[, function(){ return 0; }]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteCodeWScope) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false),
                  "[CodeWScope( , {}), CodeWScope( this.b == c, { c: 1 })]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteMinKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << MINKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MinKey, MinKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteMaxKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lte" << MAXKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MinKey, MaxKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lt: 1}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtNumberMin) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << numberMin));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << negativeInfinity << "" << numberMin), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtNegativeInfinity) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lt: -Infinity}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtDate) {
    auto testIndex = buildSimpleIndexEntry();
    const auto date = Date_t::fromMillisSinceEpoch(5000);
    BSONObj obj = BSON("a" << LT << date);
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << Date_t::min() << "" << date), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtObject) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lt: {b: 1}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': {}, '': {b: 1}}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtCode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << BSONCode("function(){ return 0; }")));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[, function(){ return 0; })");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtCodeWScope) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false),
                  "[CodeWScope( , {}), CodeWScope( this.b == c, { c: 1 }))");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

// Nothing can be less than MinKey so the resulting index bounds would be a useless empty range.
TEST_F(IndexBoundsBuilderTest, TranslateLtMinKeyDoesNotGenerateBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << MINKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtMaxKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$lt" << MAXKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MinKey, MaxKey)");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtTimestamp) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << GT << Timestamp(2, 3));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  //  Constant below is ~0U, or 2**32 - 1, but cannot be written that way in JS
                  oil.intervals[0].compare(Interval(
                      fromjson("{'': Timestamp(2, 3), '': Timestamp(4294967295, 4294967295)}"),
                      false,
                      true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: 1}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 1, '': Infinity}"), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtNumberMax) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << numberMax));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << numberMax << "" << positiveInfinity), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtPositiveInfinity) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: Infinity}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtString) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: 'abc'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'abc', '': {}}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtObject) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: {b: 1}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': {b: 1}, '': []}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtCode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << BSONCode("function(){ return 0; }")));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "(function(){ return 0; }, CodeWScope( , {}))");
    ASSERT_FALSE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtCodeWScope) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "(CodeWScope( this.b == c, { c: 1 }), MaxKey)");
    ASSERT_FALSE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtMinKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << MINKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "(MinKey, MaxKey]");
    ASSERT_FALSE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

// Nothing can be greater than MaxKey so the resulting index bounds would be a useless empty range.
TEST_F(IndexBoundsBuilderTest, TranslateGtMaxKeyDoesNotGenerateBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gt" << MAXKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gte: 1}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 1, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteNumberMax) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << numberMax));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << numberMax << "" << positiveInfinity), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtePositiveInfinity) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gte: Infinity}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': Infinity, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteObject) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gte: {b: 1}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': {b: 1}, '': []}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteCode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << BSONCode("function(){ return 0; }")));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[function(){ return 0; }, CodeWScope( , {}))");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteCodeWScope) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[CodeWScope( this.b == c, { c: 1 }), MaxKey)");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_FALSE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteMinKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << MINKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MinKey, MaxKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteMaxKey) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$gte" << MAXKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MaxKey, MaxKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateEqualNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: NaN}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lt: NaN}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$lte: NaN}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gt: NaN}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteNan) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$gte: NaN}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateEqual) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << 4);
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 4, '': 4}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateExprEqual) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprEq" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 4, '': 4}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateExprEqualToStringRespectsCollation) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << BSON("$_internalExprEq" << "foo"));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateExprEqualHashedIndex) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprEq" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

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
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateArrayEqualBasic) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: [1, 2, 3]}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': [1, 2, 3], '': [1, 2, 3]}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateIn) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$in: [8, 44, -1, -3]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
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
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInArray) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$in: [[1], 2]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 3U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[2].compare(Interval(fromjson("{'': [1], '': [1]}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteBinData) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson(
        "{a: {$lte: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA',"
        "$type: '00'}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '', $type: '00'},"
                                    "'': {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}"),
                           true,
                           true)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtBinData) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson(
        "{a: {$lt: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA',"
        "$type: '00'}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '', $type: '00'},"
                                    "'': {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}"),
                           true,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtBinData) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson(
        "{a: {$gt: {$binary: '////////////////////////////',"
        "$type: '00'}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '////////////////////////////', $type: '00'},"
                                    "'': ObjectId('000000000000000000000000')}"),
                           false,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteBinData) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson(
        "{a: {$gte: {$binary: '////////////////////////////',"
        "$type: '00'}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '////////////////////////////', $type: '00'},"
                                    "'': ObjectId('000000000000000000000000')}"),
                           true,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteArrayWithElementSmallerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gte: [1]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(maxKeyIntObj(1), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteArrayWithElementLargerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gte: [true]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendArray("", fromjson("[true]"));
    bob.appendMaxKey("");

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteEmptyArray) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gte: []}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteNestedArrayWithFirstElementSmallerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gte: [[1]]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendArray("", fromjson("[1]"));  // Same as input, but 1 level less deep.
    bob.appendMaxKey("");

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGteNestedArrayWithFirstElementLargerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gte: [[true]]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendArray("", fromjson("[[true]]"));  // Same as input.
    bob.appendMaxKey("");

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtArrayWithElementSmallerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gt: [1]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(maxKeyIntObj(1), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtArrayWithElementLargerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gt: [true]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendArray("", fromjson("[true]"));
    bob.appendMaxKey("");

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtEmptyArray) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gt: []}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtNestedArrayWithFirstElementSmallerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gt: [[1]]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendArray("", fromjson("[1]"));  // Same as input, but 1 level less deep.
    bob.appendMaxKey("");

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGtNestedArrayWithFirstElementLargerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$gt: [[true]]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendArray("", fromjson("[[true]]"));  // Same as input.
    bob.appendMaxKey("");

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteArrayWithElementSmallerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lte: [1]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendArray("", fromjson("[1.0]"));

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteArrayWithElementLargerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lte: [true]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendBool("", true);

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteEmptyArray) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lte: []}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendArray("", BSONObj());

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteNestedArrayWithFirstElementSmallerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lte: [[1]]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendArray("", fromjson("[[1.0]]"));

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLteNestedArrayWithFirstElementLargerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lte: [[true]]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendArray("", fromjson("[true]"));

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtArrayWithElementSmallerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lt: [1]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendArray("", fromjson("[1.0]"));

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtArrayWithElementLargerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lt: [true]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendBool("", true);

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtEmptyArray) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lt: []}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendArray("", BSONObj());

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtNestedArrayWithFirstElementSmallerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lt: [[1]]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendArray("", fromjson("[[1.0]]"));

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLtNestedArrayWithFirstElementLargerThanPredicate) {
    auto testIndex = buildMultikeyIndexEntry(BSON("a" << 1), {{0U}, {}});
    BSONObj obj = fromjson("{a: {$lt: [[true]]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendArray("", fromjson("[true]"));

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(bob.obj(), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

//
// $exists tests
//

TEST_F(IndexBoundsBuilderTest, ExistsTrue) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, ExistsFalse) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: false}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, ExistsTrueSparse) {
    auto keyPattern = BSONObj();
    IndexEntry testIndex =
        IndexEntry(keyPattern,
                   IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                   IndexConfig::kLatestIndexVersion,
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
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
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
    // Disable normalization, otherwise it would be normalized into {$in: [1, 5]} expression.
    testTranslateAndUnion(toUnion, &oil, &tightness, false);
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
    std::vector<BSONObj> constraints;
    constraints.push_back(fromjson("{a: {$gt: 1}}"));
    constraints.push_back(fromjson("{a: {$lte: 0}}"));
    constraints.push_back(fromjson("{a: {$in:[]}}"));
    auto intersectObj = BSON("$and" << BSON_ARRAY(constraints[0] << constraints[1]));
    auto orObj = BSON("$or" << BSON_ARRAY(intersectObj << constraints[2]));
    auto [orExpr, inputParamIdMap] = parseMatchExpression(orObj, false);

    // Decompose the expression and make sure the structure of the expression is as expected.
    ASSERT_EQ(MatchExpression::OR, orExpr->matchType()) << orExpr->debugString();
    ASSERT_EQ(2, orExpr->numChildren()) << orExpr->debugString();

    const MatchExpression* andExpr = orExpr->getChild(0)->matchType() == MatchExpression::AND
        ? orExpr->getChild(0)
        : orExpr->getChild(1);
    ASSERT_EQ(2, andExpr->numChildren()) << andExpr->debugString();

    const MatchExpression* leafExpr = orExpr->getChild(0)->matchType() == MatchExpression::AND
        ? orExpr->getChild(1)
        : orExpr->getChild(0);
    ASSERT_EQ(0, leafExpr->numChildren()) << leafExpr->debugString();

    BSONElement elt = constraints[0].firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(
        andExpr->getChild(0), elt, testIndex, &oil, &tightness, &ietBuilder);
    IndexBoundsBuilder::translateAndIntersect(
        andExpr->getChild(1), elt, testIndex, &oil, &tightness, &ietBuilder);
    IndexBoundsBuilder::translateAndUnion(leafExpr, elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);

    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
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
    auto obj = BSON("$and" << toIntersect);
    testTranslateAndIntersect(toIntersect, &oil, &tightness, obj);
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
    toIntersect.push_back(fromjson("{a: 1}"));
    toIntersect.push_back(fromjson("{a: {$gte: 1}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    auto obj = BSON("$and" << toIntersect);
    testTranslateAndIntersect(toIntersect, &oil, &tightness, obj);
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
    auto obj = BSON("$and" << toIntersect);
    testTranslateAndIntersect(toIntersect, &oil, &tightness, obj);
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
    auto obj = BSON("$and" << toIntersect);
    testTranslateAndIntersect(toIntersect, &oil, &tightness, obj);
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
    auto obj = BSON("$and" << toIntersect);
    testTranslateAndIntersect(toIntersect, &oil, &tightness, obj);
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
    auto obj = BSON("$and" << toIntersect);
    testTranslateAndIntersect(toIntersect, &oil, &tightness, obj);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 6, '': 13}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, EmptyIntersection) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: 1}"));
    toIntersect.push_back(fromjson("{a: {$gte: 2}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    auto obj = BSON("$and" << toIntersect);
    testTranslateAndIntersect(toIntersect, &oil, &tightness, obj);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
}

//
// $mod
//

TEST_F(IndexBoundsBuilderTest, TranslateMod) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$mod: [2, 0]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << NaN << "" << positiveInfinity), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

//
// Complementing bounds for negations
//

// Expected oil: [MinKey, 3), (3, MaxKey]
TEST_F(IndexBoundsBuilderTest, SimpleNE) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = BSON("a" << BSON("$ne" << 3));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(3), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(maxKeyIntObj(3), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, IntersectWithNE) {
    auto testIndex = buildSimpleIndexEntry();
    std::vector<BSONObj> toIntersect;
    toIntersect.push_back(fromjson("{a: {$gt: 1}}"));
    toIntersect.push_back(fromjson("{a: {$ne: 2}}"));
    toIntersect.push_back(fromjson("{a: {$lte: 6}}"));
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    auto obj = BSON("$and" << toIntersect);
    testTranslateAndIntersect(toIntersect, &oil, &tightness, obj);
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
    toUnionize.push_back(fromjson("{a: {$ne: 4}}"));
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
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TypeStringOrNumberHasCorrectBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: ['string', 'number']}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': Infinity}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, RedundantTypeNumberHasCorrectBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: ['number', 'int', 'long', 'double']}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': Infinity}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, CanUseCoveredMatchingForEqualityPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$eq: 3}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    ASSERT_TRUE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForEqualityToArrayPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$eq: [1, 2, 3]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForEqualityToNullPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: null}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForTypeArrayPredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'array'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForExistsTruePredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CannotUseCoveredMatchingForExistsFalsePredicate) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$exists: false}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    ASSERT_FALSE(IndexBoundsBuilder::canUseCoveredMatching(expr.get(), testIndex));
}

TEST_F(IndexBoundsBuilderTest, CanUseCoveredMatchingForExistsTrueWithSparseIndex) {
    auto testIndex = buildSimpleIndexEntry();
    testIndex.sparse = true;
    BSONObj obj = fromjson("{a: {$exists: true}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
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

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGT) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprGt" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(maxKeyIntObj(4), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTNull) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprGt" << BSONNULL));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "(null, MaxKey]");
    ASSERT_FALSE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTMaxKeyDoesNotGenerateBounds) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprGt" << MAXKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

DEATH_TEST_F(IndexBoundsBuilderTest,
             TranslateInternalExprGTMultikeyPathFails,
             "$expr comparison predicates on multikey paths cannot use an index") {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildMultikeyIndexEntry(keyPattern, {{0U}, {}});
    BSONObj obj = BSON("a" << BSON("$_internalExprGt" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTNonMultikeyPathOnMultikeyIndexSucceeds) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildMultikeyIndexEntry(keyPattern, {{}, {0U}});
    BSONObj obj = BSON("a" << BSON("$_internalExprGt" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(maxKeyIntObj(4), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTSubObjectContainingBadValuesSucceeds) {
    BSONObj keyPattern = BSON("_id" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj subObj = BSON("subObj" << BSON("a" << BSONUndefined << "b" << BSON_ARRAY("array")));
    BSONObj obj = BSON("_id" << BSON("$_internalExprGt" << subObj));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "_id");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << subObj << "" << MAXKEY), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTE) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprGte" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(maxKeyIntObj(4), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTENull) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprGte" << BSONNULL));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[null, MaxKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTEMaxKeyGeneratesBounds) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprGte" << MAXKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MaxKey, MaxKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

DEATH_TEST_F(IndexBoundsBuilderTest,
             TranslateInternalExprGTEMultikeyPathFails,
             "$expr comparison predicates on multikey paths cannot use an index") {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildMultikeyIndexEntry(keyPattern, {{0U}, {}});
    BSONObj obj = BSON("a" << BSON("$_internalExprGte" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTENonMultikeyPathOnMultikeyIndexSucceeds) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildMultikeyIndexEntry(keyPattern, {{}, {0U}});
    BSONObj obj = BSON("a" << BSON("$_internalExprGte" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(maxKeyIntObj(4), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprGTESubObjectContainingBadValuesSucceeds) {
    BSONObj keyPattern = BSON("_id" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj subObj = BSON("subObj" << BSON("a" << BSONUndefined << "b" << BSON_ARRAY("array")));
    BSONObj obj = BSON("_id" << BSON("$_internalExprGte" << subObj));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "_id");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << subObj << "" << MAXKEY), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLT) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprLt" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(4), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTNull) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprLt" << BSONNULL));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MinKey, null]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTMinKeyDoesNotGenerateBounds) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprLt" << MINKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

DEATH_TEST_F(IndexBoundsBuilderTest,
             TranslateInternalExprLTMultikeyPathFails,
             "$expr comparison predicates on multikey paths cannot use an index") {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildMultikeyIndexEntry(keyPattern, {{0U}, {}});
    BSONObj obj = BSON("a" << BSON("$_internalExprLt" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTNonMultikeyPathOnMultikeyIndexSucceeds) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildMultikeyIndexEntry(keyPattern, {{}, {0U}});
    BSONObj obj = BSON("a" << BSON("$_internalExprLt" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(4), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTSubObjectContainingBadValuesSucceeds) {
    BSONObj keyPattern = BSON("_id" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj subObj = BSON("subObj" << BSON("a" << BSONUndefined << "b" << BSON_ARRAY("array")));
    BSONObj obj = BSON("_id" << BSON("$_internalExprLt" << subObj));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "_id");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << MINKEY << "" << subObj), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTE) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprLte" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(4), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTENull) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprLte" << BSONNULL));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MinKey, null]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTEMinKeyGeneratesBounds) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprLte" << MINKEY));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(oil.intervals[0].toString(false), "[MinKey, MinKey]");
    ASSERT_TRUE(oil.intervals[0].startInclusive);
    ASSERT_TRUE(oil.intervals[0].endInclusive);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

DEATH_TEST_F(IndexBoundsBuilderTest,
             TranslateInternalExprLTEMultikeyPathFails,
             "$expr comparison predicates on multikey paths cannot use an index") {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildMultikeyIndexEntry(keyPattern, {{0U}, {}});

    BSONObj obj = BSON("a" << BSON("$_internalExprLte" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTENonMultikeyPathOnMultikeyIndexSucceeds) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildMultikeyIndexEntry(keyPattern, {{}, {0U}});
    BSONObj obj = BSON("a" << BSON("$_internalExprLte" << 4));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(4), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateInternalExprLTESubObjectContainingBadValuesSucceeds) {
    BSONObj keyPattern = BSON("_id" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj subObj = BSON("subObj" << BSON("a" << BSONUndefined << "b" << BSON_ARRAY("array")));
    BSONObj obj = BSON("_id" << BSON("$_internalExprLte" << subObj));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "_id");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << MINKEY << "" << subObj), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateBoundsForWildcardIndexes) {
    BSONObj keyPattern = BSON("a" << 1);
    auto testIndex = buildWildcardIndexEntry(keyPattern, {{}});
    BSONObj obj = fromjson("{a: {$lte: 1}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateBoundsForCompoundWildcardIndexes) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    auto testIndex = buildWildcardIndexEntry(keyPattern, {{}}, 0);
    BSONObj obj = fromjson("{a: {$lt: 1}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, AdjustIndexBoundsForWildcardIndexesIfObjectIncluded) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    auto testIndex = buildWildcardIndexEntry(keyPattern, {{}}, 0);
    BSONObj obj = fromjson("{a: {$eq: {a: 1}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << MINKEY << "" << MAXKEY), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}
}  // namespace
