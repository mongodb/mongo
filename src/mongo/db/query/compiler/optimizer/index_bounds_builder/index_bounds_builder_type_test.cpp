// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder_test_fixture.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/interval_evaluation_tree.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <vector>

namespace mongo {
namespace {

using DoubleLimits = std::numeric_limits<double>;

TEST_F(IndexBoundsBuilderTest, TypeNumber) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'number'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    // Build the expected interval.
    BSONObjBuilder bob;
    BSONType type = BSONType::numberInt;
    bob.appendMinForType("", stdx::to_underlying(type));
    bob.appendMaxForType("", stdx::to_underlying(type));
    BSONObj expectedInterval = bob.obj();

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

// Test $type bounds for Code BSON type.
TEST_F(IndexBoundsBuilderTest, CodeTypeBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 13}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

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
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

// Test $type bounds for Code With Scoped BSON type.
TEST_F(IndexBoundsBuilderTest, CodeWithScopeTypeBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 15}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

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
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

// Test $type bounds for double BSON type.
TEST_F(IndexBoundsBuilderTest, DoubleTypeBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 1}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

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
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TypeArrayBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'array'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TypeSymbolBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'symbol'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TypeStringWithoutCollatorBounds) {
    auto testIndex = buildSimpleIndexEntry();

    BSONObj obj = fromjson("{a: {$type: 'string'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TypeSymbolAndStringBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: ['string', 'symbol']}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TypeNullBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'null'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TypeUndefinedBounds) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: {$type: 'undefined'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': undefined, '': undefined}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}


}  // namespace
}  // namespace mongo
