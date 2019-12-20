/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

namespace mongo {
namespace {

using DoubleLimits = std::numeric_limits<double>;

TEST_F(IndexBoundsBuilderTest, TypeNumber) {
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

// Test $type bounds for Code BSON type.
TEST_F(IndexBoundsBuilderTest, CodeTypeBounds) {
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
                  oil.intervals[0].compare(Interval(expectedInterval, true, false)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

// Test $type bounds for Code With Scoped BSON type.
TEST_F(IndexBoundsBuilderTest, CodeWithScopeTypeBounds) {
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
                  oil.intervals[0].compare(Interval(expectedInterval, true, false)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

// Test $type bounds for double BSON type.
TEST_F(IndexBoundsBuilderTest, DoubleTypeBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 1}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Build the expected interval.
    BSONObjBuilder bob;
    bob.appendNumber("", DoubleLimits::quiet_NaN());
    bob.appendNumber("", DoubleLimits::infinity());
    BSONObj expectedInterval = bob.obj();

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, TypeArrayBounds) {
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

TEST_F(IndexBoundsBuilderTest, TypeSymbolBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'symbol'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, TypeStringWithoutCollatorBounds) {
    auto testIndex = buildSimpleIndexEntry();

    BSONObj obj = fromjson("{a: {$type: 'string'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, TypeSymbolAndStringBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: ['string', 'symbol']}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, TypeNullBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'null'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST_F(IndexBoundsBuilderTest, TypeUndefinedBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'undefined'}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': undefined, '': undefined}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}


}  // namespace
}  // namespace mongo
