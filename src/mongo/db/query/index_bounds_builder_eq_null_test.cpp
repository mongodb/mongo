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

#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/expression_index.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_bounds_builder_test.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/interval.h"
#include "mongo/db/query/interval_evaluation_tree.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

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

TEST_F(IndexBoundsBuilderTest, TranslateExprEqualToNullIsExactMaybeCovered) {
    BSONObj keyPattern = BSON("a" << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = BSON("a" << BSON("$_internalExprEq" << BSONNULL));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': undefined, '': undefined}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT_MAYBE_COVERED);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateEqualsToNullShouldBuildExactMaybeCoveredBounds) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    BSONObj obj = BSON("a" << BSONNULL);
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT_MAYBE_COVERED);
    assertBoundsRepresentEqualsNull(oil);
    assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateDottedEqualsToNullShouldBuildExactMaybeCoveredBounds) {
    BSONObj indexPattern = BSON("a.b" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    BSONObj obj = BSON("a.b" << BSONNULL);
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a.b");
    // Depending on the query projection, this will either be converted to EXACT or to INEXACT_FETCH
    // before we build an IXSCAN plan.
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT_MAYBE_COVERED);
    assertBoundsRepresentEqualsNull(oil);
    assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateEqualsToNullMultiKeyShouldBuildInexactBounds) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    BSONObj obj = BSON("a" << BSONNULL);
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentEqualsNull(oil);
    assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, TranslateEqualsToNullShouldBuildTwoIntervalsForHashedIndex) {
    BSONObj indexPattern = BSON("a"
                                << "hashed");
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.type = IndexType::INDEX_HASHED;

    BSONObj obj = BSON("a" << BSONNULL);
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

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
    assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
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

const std::vector<BSONObj> kNeNullQueries = {BSON("a" << BSON("$ne" << BSONNULL)),
                                             BSON("a" << BSON("$not" << BSON("$lte" << BSONNULL))),
                                             BSON("a" << BSON("$not" << BSON("$gte" << BSONNULL)))};

TEST_F(IndexBoundsBuilderTest, TranslateNotEqualToNullShouldBuildExactBoundsIfIndexIsNotMultiKey) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    for (const BSONObj& obj : kNeNullQueries) {
        // It's necessary to call optimize since the $not will have a singleton $and child, which
        // IndexBoundsBuilder::translate cannot handle.
        auto [expr, inputParamIdMap] = parseMatchExpression(obj);
        expr = MatchExpression::optimize(std::move(expr));

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};
        IndexBoundsBuilder::translate(
            expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

        // Bounds should be [MinKey, undefined), (null, MaxKey].
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
        assertBoundsRepresentNotEqualsNull(oil);
        assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
    }
}

TEST_F(IndexBoundsBuilderTest,
       TranslateNotEqualToNullShouldBuildExactBoundsIfIndexIsNotMultiKeyOnRelevantPath) {
    BSONObj indexPattern = BSON("a" << 1 << "b" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikeyPaths = {MultikeyComponents{},
                               MultikeyComponents{0}};  // "a" is not multi-key, but "b" is.

    for (const BSONObj& obj : kNeNullQueries) {
        // It's necessary to call optimize since the $not will have a singleton $and child, which
        // IndexBoundsBuilder::translate cannot handle.
        auto [expr, inputParamIdMap] = parseMatchExpression(obj);
        expr = MatchExpression::optimize(std::move(expr));

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};
        IndexBoundsBuilder::translate(
            expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

        // Bounds should be [MinKey, undefined), (null, MaxKey].
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
        assertBoundsRepresentNotEqualsNull(oil);
        assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
    }
}

TEST_F(IndexBoundsBuilderTest, TranslateNotEqualToNullShouldBuildExactBoundsOnReverseIndex) {
    BSONObj indexPattern = BSON("a" << -1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    for (const BSONObj& obj : kNeNullQueries) {
        // It's necessary to call optimize since the $not will have a singleton $and child, which
        // IndexBoundsBuilder::translate cannot handle.
        auto [expr, inputParamIdMap] = parseMatchExpression(obj);
        expr = MatchExpression::optimize(std::move(expr));

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};
        IndexBoundsBuilder::translate(
            expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

        // Bounds should be [MinKey, undefined), (null, MaxKey].
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
        assertBoundsRepresentNotEqualsNull(oil);
        assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
    }
}

TEST_F(IndexBoundsBuilderTest, TranslateNotEqualToNullShouldBuildInexactBoundsIfIndexIsMultiKey) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    for (const BSONObj& obj : kNeNullQueries) {
        // It's necessary to call optimize since the $not will have a singleton $and child, which
        // IndexBoundsBuilder::translate cannot handle.
        auto [expr, inputParamIdMap] = parseMatchExpression(obj);
        expr = MatchExpression::optimize(std::move(expr));

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};
        IndexBoundsBuilder::translate(
            expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
        assertBoundsRepresentNotEqualsNull(oil);
        assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
    }
}

TEST_F(IndexBoundsBuilderTest, TranslateInequalityToNullShouldProduceExactEmptyBounds) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    const std::vector<BSONObj> inequalities = {BSON("a" << BSON("$lt" << BSONNULL)),
                                               BSON("a" << BSON("$gt" << BSONNULL))};

    for (const BSONObj& obj : inequalities) {
        // It's necessary to call optimize since the $not will have a singleton $and child, which
        // IndexBoundsBuilder::translate cannot handle.
        auto [expr, inputParamIdMap] = parseMatchExpression(obj);

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};
        IndexBoundsBuilder::translate(
            expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
        ASSERT(oil.intervals.empty());
        assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
    }
}

TEST_F(IndexBoundsBuilderTest, TranslateNotInequalityToNullShouldProduceExactFullBounds) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    const std::vector<BSONObj> inequalities = {
        BSON("a" << BSON("$not" << BSON("$lt" << BSONNULL))),
        BSON("a" << BSON("$not" << BSON("$gt" << BSONNULL)))};

    for (const BSONObj& obj : inequalities) {
        // It's necessary to call optimize since the $not will have a singleton $and child, which
        // IndexBoundsBuilder::translate cannot handle.
        auto [expr, inputParamIdMap] = parseMatchExpression(obj);
        expr = MatchExpression::optimize(std::move(expr));

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};
        IndexBoundsBuilder::translate(
            expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
        ASSERT_EQ(oil.intervals.size(), 1);
        ASSERT(oil.intervals.front().isMinToMax());
        assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
    }
}

TEST_F(IndexBoundsBuilderTest,
       TranslateNotInequalityToNullOnMultiKeyIndexShouldProduceInexactFullBounds) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    const std::vector<BSONObj> inequalities = {
        BSON("a" << BSON("$not" << BSON("$lt" << BSONNULL))),
        BSON("a" << BSON("$not" << BSON("$gt" << BSONNULL)))};

    for (const BSONObj& obj : inequalities) {
        // It's necessary to call optimize since the $not will have a singleton $and child, which
        // IndexBoundsBuilder::translate cannot handle.
        auto [expr, inputParamIdMap] = parseMatchExpression(obj);
        expr = MatchExpression::optimize(std::move(expr));

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};
        IndexBoundsBuilder::translate(
            expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
        ASSERT_EQ(oil.intervals.size(), 1);
        ASSERT(oil.intervals.front().isMinToMax());
        assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
    }
}

TEST_F(IndexBoundsBuilderTest,
       TranslateDottedElemMatchValueNotEqualToNullShouldBuildExactBoundsIfIsMultiKeyOnThatPath) {
    BSONObj indexPattern = BSON("a.b" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikeyPaths = {{1}};  // "a.b" is multikey.

    BSONObj matchObj = BSON("a.b" << BSON("$elemMatch" << BSON("$ne" << BSONNULL)));
    auto [expr, inputParamIdMap] = parseMatchExpression(matchObj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a.b");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
    assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest,
       TranslateDottedFieldNotEqualToNullShouldBuildInexactBoundsIfIndexIsMultiKey) {
    BSONObj indexPattern = BSON("a.b" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    BSONObj matchObj = BSON("a.b" << BSON("$ne" << BSONNULL));
    auto [expr, inputParamIdMap] = parseMatchExpression(matchObj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a.b");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
    assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest,
       TranslateElemMatchValueNotEqualToNullShouldBuildInexactBoundsIfIndexIsMultiKey) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);
    testIndex.multikey = true;

    BSONObj obj = BSON("a" << BSON("$elemMatch" << BSON("$ne" << BSONNULL)));
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest,
       TranslateElemMatchValueNotEqualToNullShouldBuildInExactBoundsIfIndexIsNotMultiKey) {
    BSONObj indexPattern = BSON("a" << 1);
    auto testIndex = buildSimpleIndexEntry(indexPattern);

    BSONObj matchObj = BSON("a" << BSON("$elemMatch" << BSON("$ne" << BSONNULL)));
    auto [expr, inputParamIdMap] = parseMatchExpression(matchObj);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(
        expr.get(), indexPattern.firstElement(), testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    assertBoundsRepresentNotEqualsNull(oil);
    assertIET(inputParamIdMap, ietBuilder, indexPattern.firstElement(), testIndex, oil);
}

}  // namespace
}  // namespace mongo
