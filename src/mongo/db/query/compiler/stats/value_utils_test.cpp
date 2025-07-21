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

#include "mongo/db/query/compiler/stats/value_utils.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder_test_fixture.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/compiler/stats/test_utils.h"
#include "mongo/unittest/unittest.h"

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


TEST_F(ValueUtilsTest, SameTypeClassAlignsWithMinComparison) {
    auto allTypeTagPairs = generateAllTypeTagPairs();

    // Asserts that sameTypeClass() behaves the same as the alternative implementation
    // sameTypeClassByComparingMin() which compares the minimum values in BSON.
    for (auto typeTagPair : allTypeTagPairs) {
        bool lhs = sameTypeClassByComparingMin(typeTagPair.first, typeTagPair.second);
        bool rhs = sameTypeClass(typeTagPair.first, typeTagPair.second);
        ASSERT_EQ(lhs, rhs) << "first tag: " << typeTagPair.first
                            << " second tag: " << typeTagPair.second << " sameTypeClass: " << lhs
                            << " compareTypeTags: " << rhs;
    }
}

TEST_F(ValueUtilsTest, GetMinBoundAlignsWithAppendMinForType) {
    // Asserts that the behavior of getMinBound() is consistent with the BSON implementation
    // appendMinForType().
    for (size_t t = 0; t < size_t(sbe::value::TypeTags::TypeTagsMax); ++t) {
        auto tag = static_cast<sbe::value::TypeTags>(t);

        // Excludes all the extended types which are unsupported.
        auto bsonType = sbe::value::tagToType(tag);
        if (bsonType == BSONType::eoo) {
            continue;
        }
        auto [actual, inclusive] = getMinBound(tag);

        BSONObjBuilder builder;
        builder.appendMinForType("", stdx::to_underlying(bsonType));
        auto obj = builder.obj();
        auto elem = obj.firstElement();
        auto expected = sbe::bson::convertFrom<false>(elem);
        sbe::value::ValueGuard guard{expected};

        auto res = stats::compareValues(
            actual.getTag(), actual.getValue(), expected.first, expected.second);
        ASSERT_EQ(res, 0) << "tag: " << tag << ", getMinBound() returns: " << actual.get()
                          << ", expected returning: " << expected;
        ASSERT_TRUE(inclusive);
    }
}

TEST_F(ValueUtilsTest, GetMaxBoundAlignsWithAppendMaxForType) {
    // Asserts that the behavior of getMaxBound() is consistent with the BSON implementation
    // appendMaxForType().
    for (size_t t = 0; t < size_t(sbe::value::TypeTags::TypeTagsMax); ++t) {
        auto tag = static_cast<sbe::value::TypeTags>(t);

        // Excludes all the extended types which are unsupported.
        auto bsonType = sbe::value::tagToType(tag);
        if (bsonType == BSONType::eoo) {
            continue;
        }
        auto [actual, inclusive] = getMaxBound(tag);

        BSONObjBuilder builder;
        builder.appendMaxForType("", stdx::to_underlying(bsonType));
        auto obj = builder.obj();
        auto elem = obj.firstElement();
        auto expected = sbe::bson::convertFrom<false>(elem);
        sbe::value::ValueGuard guard{expected};

        auto res = stats::compareValues(
            actual.getTag(), actual.getValue(), expected.first, expected.second);
        ASSERT_EQ(res, 0) << "tag: " << tag << ", getMaxBound() returns: " << actual.get()
                          << ", expected returning: " << expected;
        ASSERT_EQ(inclusive, !isVariableWidthType(tag));
    }
}

TEST_F(ValueUtilsTest, CanEstimateTypeViaTypeCounts) {
    // Asserts that canEstimateTypeViaTypeCounts() returns true if the type has only single possible
    // value, or the type is boolean. Otherwise, the function returns false.
    for (size_t t = 0; t < size_t(sbe::value::TypeTags::TypeTagsMax); ++t) {
        auto tag = static_cast<sbe::value::TypeTags>(t);

        // Excludes all the extended types which are unsupported.
        auto bsonType = sbe::value::tagToType(tag);
        if (bsonType == BSONType::eoo) {
            ASSERT_FALSE(stats::canEstimateTypeViaTypeCounts(tag));
            continue;
        }
        auto [min, minInclusive] = getMinBound(tag);
        auto [max, maxInclusive] = getMaxBound(tag);
        if (min == max || tag == sbe::value::TypeTags::Boolean) {
            ASSERT_TRUE(stats::canEstimateTypeViaTypeCounts(tag));
        } else {
            ASSERT_FALSE(stats::canEstimateTypeViaTypeCounts(tag));
        }
    }
}

TEST_F(ValueUtilsTest, ReturnsTrueForFullBracketIntervals) {
    // Asserts that isFullBracketInterval() returns true for all full bracket intervals of each
    // BSONType.
    for (int t = -1; t < stdx::to_underlying(BSONType::maxKey); ++t) {
        if (!isValidBSONType(t) || t == stdx::to_underlying(BSONType::eoo) ||
            t == stdx::to_underlying(BSONType::array)) {
            continue;
        }
        BSONObj obj = BSON("a" << BSON("$type" << t));
        auto interval = buildInterval(obj);

        // Converts to SBE values.
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
        sbe::value::ValueGuard startGuard{startTag, startVal};
        sbe::value::ValueGuard endGuard{endTag, endVal};

        ASSERT_TRUE(
            isFullBracketInterval(startTag, startVal, startInclusive, endTag, endVal, endInclusive))
            << "type: " << typeName(BSONType(t));
    }
}

TEST_F(ValueUtilsTest, ReturnsFalseForNonFullBracketIntervalsWithStaticWidthType) {
    auto [start, startInclusive] = getMinBound(sbe::value::TypeTags::NumberInt32);
    auto [end, endInclusive] = getMaxBound(sbe::value::TypeTags::NumberInt32);

    // Asserts that the interval [start, end] can be considered as a full bracket
    // interval if inclusiveness are both inclusive.
    ASSERT_TRUE(isFullBracketInterval(start.getTag(),
                                      start.getValue(),
                                      startInclusive,
                                      end.getTag(),
                                      end.getValue(),
                                      endInclusive));

    // Asserts that the function returns false if either bound is exclusive.
    ASSERT_FALSE(isFullBracketInterval(start.getTag(),
                                       start.getValue(),
                                       false /*startInclusive*/,
                                       end.getTag(),
                                       end.getValue(),
                                       false /*endInclusive*/));

    ASSERT_FALSE(isFullBracketInterval(start.getTag(),
                                       start.getValue(),
                                       true /*startInclusive*/,
                                       end.getTag(),
                                       end.getValue(),
                                       false /*endInclusive*/));

    ASSERT_FALSE(isFullBracketInterval(start.getTag(),
                                       start.getValue(),
                                       false /*startInclusive*/,
                                       end.getTag(),
                                       end.getValue(),
                                       true /*endInclusive*/));
}

TEST_F(ValueUtilsTest, ReturnsFalseForNonFullBracketIntervalsWithVariableWidthType) {
    auto [start, startInclusive] = getMinBound(sbe::value::TypeTags::StringSmall);
    auto [end, endInclusive] = getMaxBound(sbe::value::TypeTags::StringSmall);

    // Asserts that the interval [start, end] can be considered as a full bracket
    // interval if inclusiveness are both inclusive.
    ASSERT_TRUE(isFullBracketInterval(start.getTag(),
                                      start.getValue(),
                                      startInclusive,
                                      end.getTag(),
                                      end.getValue(),
                                      endInclusive));

    // Asserts that the function returns false if 'endInclusive' is true.
    ASSERT_FALSE(isFullBracketInterval(start.getTag(),
                                       start.getValue(),
                                       startInclusive /*startInclusive*/,
                                       end.getTag(),
                                       end.getValue(),
                                       true /*endInclusive*/));

    // Asserts that the function returns false the end bound is incorrect.
    ASSERT_FALSE(isFullBracketInterval(start.getTag(),
                                       start.getValue(),
                                       true /*startInclusive*/,
                                       start.getTag(),
                                       start.getValue(),
                                       endInclusive /*endInclusive*/));

    // Asserts that the short-circuit returns false when the function detects the end tag is not the
    // next type of the start tag.
    ASSERT_FALSE(isFullBracketInterval(start.getTag(),
                                       start.getValue(),
                                       startInclusive,
                                       sbe::value::TypeTags::Array,
                                       end.getValue(),
                                       endInclusive));
}

TEST_F(ValueUtilsTest, BracketizeSingleTypeInterval) {
    using TypeTags = sbe::value::TypeTags;
    auto [startTag, startVal] = makeInt64Value(100);
    auto [endTag, endVal] = makeInt64Value(200);
    auto intervals = bracketizeInterval(
        startTag, startVal, false /*startInclusive*/, endTag, endVal, false /*endInclusive*/);

    std::pair<std::pair<SBEValue, bool>, std::pair<SBEValue, bool>> expected{
        {SBEValue{makeInt64Value(100)}, false}, {SBEValue{makeInt64Value(200)}, false}};

    // Asserts that if the interval is single-type interval, it will not be bracketized. The
    // returned sub-intervals will contain exactly one interval which is the itself.
    ASSERT_EQ(intervals.size(), 1);
    ASSERT_EQ(intervals[0], expected);
}

TEST_F(ValueUtilsTest, BracketizeSameTypeBracketInterval) {
    using TypeTags = sbe::value::TypeTags;
    // This test checks if bracketizeInterval can correctly handle the interval (100, ""), which is
    // equivalent of (100, inf]. Despite the different type tags, (100, "") is still of the same
    // type bracket.
    auto [startTag, startVal] = makeInt64Value(100);
    auto [endTag, endVal] = sbe::value::makeNewString("");
    auto intervals = bracketizeInterval(
        startTag, startVal, false /*startInclusive*/, endTag, endVal, false /*endInclusive*/);

    std::pair<std::pair<SBEValue, bool>, std::pair<SBEValue, bool>> expected{
        {SBEValue{makeInt64Value(100)}, false}, {sbe::value::makeNewString(""), false}};

    // Asserts that if the interval is a of the same type bracket, it will not be bracketized. The
    // returned sub-intervals will contain exactly one interval which is the itself.
    ASSERT_EQ(intervals.size(), 1);
    ASSERT_EQ(intervals[0], expected);
}

TEST_F(ValueUtilsTest, BracketizeMixedTypeInterval) {
    using TypeTags = sbe::value::TypeTags;
    auto [startTag, startVal] = makeInt64Value(100);
    auto [endTag, endVal] = makeBooleanValue(1);
    auto intervals = bracketizeInterval(
        startTag, startVal, false /*startInclusive*/, endTag, endVal, false /*endInclusive*/);

    std::vector<std::pair<std::pair<SBEValue, bool>, std::pair<SBEValue, bool>>> expected = {
        {{SBEValue{makeInt64Value(100)}, false}, getMaxBound(TypeTags::NumberInt64)},
        {getMinBound(TypeTags::StringSmall), getMaxBound(TypeTags::StringSmall)},
        {getMinBound(TypeTags::Object), getMaxBound(TypeTags::Object)},
        {getMinBound(TypeTags::Array), getMaxBound(TypeTags::Array)},
        {getMinBound(TypeTags::bsonBinData), getMaxBound(TypeTags::bsonBinData)},
        {getMinBound(TypeTags::ObjectId), getMaxBound(TypeTags::ObjectId)},
        {getMinBound(TypeTags::Boolean), {SBEValue{makeBooleanValue(1)}, false}},
    };
    for (size_t i = 0; i < intervals.size(); ++i) {
        ASSERT_EQ(intervals[i], expected[i]);
    }
}

TEST_F(ValueUtilsTest, BracketizeEmptyInterval) {
    using TypeTags = sbe::value::TypeTags;
    auto [startTag, startVal] = makeInt64Value(100);
    auto [endTag, endVal] = makeInt64Value(100);
    auto intervals = bracketizeInterval(
        startTag, startVal, false /*startInclusive*/, endTag, endVal, true /*endInclusive*/);

    // (100, 100] is an empty interval as no number is included. We expect the returned
    // sub-intervals to be an empty vector.
    ASSERT_EQ(intervals.size(), 0);
}

}  // namespace mongo::stats
