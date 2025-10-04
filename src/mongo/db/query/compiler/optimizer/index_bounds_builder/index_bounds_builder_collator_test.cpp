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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/expression_geo_index_mapping.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder_test_fixture.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/interval_evaluation_tree.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo {
namespace {

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

TEST_F(IndexBoundsBuilderTest, TranslateEqualityToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << "foo");
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
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateEqualityToNonStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << 3);
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 3, '': 3}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateNotEqualToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << BSON("$ne" << "bar"));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

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

    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateEqualToStringElemMatchValueWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$elemMatch: {$eq: 'baz'}}}");
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
        oil.intervals[0].compare(Interval(fromjson("{'': 'zab', '': 'zab'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLTEToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: 'foo'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLTEToNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: 3}}");
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
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 3}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLTStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: 'foo'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': 'oof'}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateLTNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: 3}}");
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
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 3}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGTStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: 'foo'}}");
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
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': {}}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGTNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: 3}}");
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
        oil.intervals[0].compare(Interval(fromjson("{'': 3, '': Infinity}"), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGTEToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: 'foo'}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': {}}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateGTEToNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: 3}}");
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
        oil.intervals[0].compare(Interval(fromjson("{'': 3, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, SimplePrefixRegexWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: /^foo/}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, NotWithMockCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$ne:  3}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(3), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(maxKeyIntObj(3), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, ExistsTrueWithMockCollatorAndSparseIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;
    testIndex.sparse = true;

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

TEST_F(IndexBoundsBuilderTest, ExistsFalseWithMockCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

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

TEST_F(IndexBoundsBuilderTest, TypeStringWithCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

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
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, InWithStringAndCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: ['foo']}}");
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
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, InWithNumberAndStringAndCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [2, 'foo']}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, InWithRegexAndCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [/^foo/]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, InWithNumberAndCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [2]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, LTEMaxKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: {$maxKey: 1}}}");
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

TEST_F(IndexBoundsBuilderTest, LTMaxKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: {$maxKey: 1}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValuesRespectingInclusion(
                      BoundInclusion::kIncludeStartKeyOnly)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, GTEMinKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: {$minKey: 1}}}");
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

TEST_F(IndexBoundsBuilderTest, GTMinKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: {$minKey: 1}}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValuesRespectingInclusion(
                      BoundInclusion::kIncludeEndKeyOnly)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, StringEqualityAgainstHashedIndexWithCollatorUsesHashOfCollationKey) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: 'foo'}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    BSONObj expectedCollationKey = BSON("" << "oof");
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
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, EqualityToNumberAgainstHashedIndexWithCollatorUsesHash) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: 3}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

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
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, InWithStringAgainstHashedIndexWithCollatorUsesHashOfCollationKey) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: ['foo']}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    BSONObj expectedCollationKey = BSON("" << "oof");
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
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

}  // namespace
}  // namespace mongo
