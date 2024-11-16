/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_bounds_builder_test_fixture.h"

#include "mongo/db/query/interval.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::stats {

using DoubleLimits = std::numeric_limits<double>;
double numberMin = -DoubleLimits::max();
double numberMax = DoubleLimits::max();
double negativeInfinity = -DoubleLimits::infinity();
double positiveInfinity = DoubleLimits::infinity();
double NaN = DoubleLimits::quiet_NaN();

class ValueUtilsTest : public unittest::Test {
public:
    static Interval buildInterval(const BSONObj& obj) {
        auto testIndex = IndexBoundsBuilderTest::buildSimpleIndexEntry();
        auto [expr, inputParamIdMap] = IndexBoundsBuilderTest::parseMatchExpression(obj);
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
        tassert(9485600,
                "buildInterval expect the query resulting in exactly one interval",
                oil.intervals.size() == 1U);
        return oil.intervals.back();
    }
};

void assertSameTypeBracketedInterval(const Interval& interval) {
    auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
    auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
    sbe::value::ValueGuard startGuard{startTag, startVal};
    sbe::value::ValueGuard endGuard{endTag, endVal};

    ASSERT(sameTypeBracketInterval(startTag, interval.endInclusive, endTag, endVal));
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalEqual) {
    BSONObj obj = BSON("a" << 4);
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  interval.compare(Interval(fromjson("{'': 4, '': 4}"), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalExistsFalse) {
    BSONObj obj = fromjson("{a: {$exists: false}}");
    auto interval = buildInterval(obj);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              interval.compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalLteNumber) {
    auto interval = buildInterval(fromjson("{a: {$lte: 1}}"));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  interval.compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalLteNumberMin) {
    BSONObj obj = BSON("a" << BSON("$lte" << numberMin));
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        interval.compare(Interval(BSON("" << negativeInfinity << "" << numberMin), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalLteNegativeInfinity) {
    BSONObj obj = fromjson("{a: {$lte: -Infinity}}");
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        interval.compare(Interval(fromjson("{'': -Infinity, '': -Infinity}"), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalLteObject) {
    BSONObj obj = fromjson("{a: {$lte: {b: 1}}}");
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  interval.compare(Interval(fromjson("{'': {}, '': {b: 1}}"), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalLteBinData) {
    BSONObj obj = fromjson(
        "{a: {$lte: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA',"
        "$type: '00'}}}");
    auto interval = buildInterval(obj);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              interval.compare(
                  Interval(fromjson("{'': {$binary: '', $type: '00'},"
                                    "'': {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}"),
                           true,
                           true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalLteCode) {
    BSONObj obj = BSON("a" << BSON("$lte" << BSONCode("function(){ return 0; }")));
    auto interval = buildInterval(obj);
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalLteCodeWScope) {
    BSONObj obj = BSON("a" << BSON("$lte" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(interval.toString(false),
                  "[CodeWScope( , {}), CodeWScope( this.b == c, { c: 1 })]");
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalGteNumber) {
    BSONObj obj = fromjson("{a: {$gte: 1}}");
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  interval.compare(Interval(fromjson("{'': 1, '': Infinity}"), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalGteNumberMax) {
    BSONObj obj = BSON("a" << BSON("$gte" << numberMax));
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        interval.compare(Interval(BSON("" << numberMax << "" << positiveInfinity), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalGtePositiveInfinity) {
    BSONObj obj = fromjson("{a: {$gte: Infinity}}");
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  interval.compare(Interval(fromjson("{'': Infinity, '': Infinity}"), true, true)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalGteObject) {
    BSONObj obj = fromjson("{a: {$gte: {b: 1}}}");
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  interval.compare(Interval(fromjson("{'': {b: 1}, '': []}"), true, false)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalGteBinData) {
    BSONObj obj = fromjson(
        "{a: {$gte: {$binary: '////////////////////////////',"
        "$type: '00'}}}");
    auto interval = buildInterval(obj);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              interval.compare(
                  Interval(fromjson("{'': {$binary: '////////////////////////////', $type: '00'},"
                                    "'': ObjectId('000000000000000000000000')}"),
                           true,
                           false)));
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalGteCode) {
    BSONObj obj = BSON("a" << BSON("$gte" << BSONCode("function(){ return 0; }")));
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(interval.toString(false), "[function(){ return 0; }, CodeWScope( , {}))");
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalGteCodeWScope) {
    BSONObj obj = BSON("a" << BSON("$gte" << BSONCodeWScope("this.b == c", BSON("c" << 1))));
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(interval.toString(false), "[CodeWScope( this.b == c, { c: 1 }), MaxKey)");
    assertSameTypeBracketedInterval(interval);
}

TEST_F(ValueUtilsTest, SameTypeBracketedIntervalGteNan) {
    BSONObj obj = fromjson("{a: {$gte: NaN}}");
    auto interval = buildInterval(obj);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  interval.compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    assertSameTypeBracketedInterval(interval);
}


}  // namespace mongo::stats
