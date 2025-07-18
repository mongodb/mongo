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
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/compiler/ce/histogram/histogram_test_utils.h"
#include "mongo/db/query/compiler/stats/max_diff.h"

namespace mongo::ce {
namespace {

namespace value = sbe::value;

using mongo::Interval;
using stats::CEHistogram;
using stats::ScalarHistogram;
using stats::TypeCounts;

auto NumberInt64 = sbe::value::TypeTags::NumberInt64;
auto StringSmall = sbe::value::TypeTags::StringSmall;

TEST(HistogramPredicateEstimationTest, CanEstimateNonHistogrammableInterval) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0}};
    const CardinalityEstimate intCnt{CardinalityType{2}, EstimationSource::Code};
    const ScalarHistogram hist = createHistogram(data);
    const auto ceHist =
        CEHistogram::make(hist, TypeCounts{{NumberInt64, intCnt.toDouble()}}, intCnt.toDouble());

    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // {a: {$gte: false}}
        Interval interval(fromjson("{'': false, '': true}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // {a: {$gte: null}}
        Interval interval(fromjson("{'': null, '':  null}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // {a: {$lte: []}}
        Interval interval(fromjson("{'': [], '': []}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // {a: {$gte: []}}
        Interval interval(fromjson("{'': [], '': {}}"), true, false);
        ASSERT_FALSE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // {a: {$exists: false}}
        Interval interval(fromjson("{'': null, '': null}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }
}

TEST(HistogramPredicateEstimationTest, CanEstimateInestimableInterval) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0}};
    const CardinalityEstimate intCnt{CardinalityType{2}, EstimationSource::Code};
    const ScalarHistogram hist = createHistogram(data);
    const auto ceHist =
        CEHistogram::make(hist, TypeCounts{{NumberInt64, intCnt.toDouble()}}, intCnt.toDouble());

    {  // {a: {b: 1}}, we cannot estimate arbitrary object intervals
        Interval interval(fromjson("{'': {b: 1}, '': {b: 1}}"), true, true);
        ASSERT_FALSE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // {a: {$gte: {b: 1}}}, we cannot estimate arbitrary object intervals, and intervals spanning
       // multiple types.
        Interval interval(fromjson("{'': {b: 1}, '':  []}"), true, false);
        ASSERT_FALSE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // The interval is not constructible from IndexBoundBuilder. We can estimate intervals
       // spanning multiple types if both bounds are estimable types.
        Interval interval(fromjson("{'': 10, '':  false}"), true, false);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }
}

TEST(HistogramPredicateEstimationTest, CanEstimateSimpleInterval) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0}};
    const CardinalityEstimate intCnt{CardinalityType{2}, EstimationSource::Code};
    const ScalarHistogram hist = createHistogram(data);
    const auto ceHist =
        CEHistogram::make(hist, TypeCounts{{NumberInt64, intCnt.toDouble()}}, intCnt.toDouble());

    {  // {a: 4}
        Interval interval(fromjson("{'': 4, '': 4}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }
}

TEST(HistogramPredicateEstimationTest, CanEstimateTypeBracketedInterval) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0}};
    const CardinalityEstimate intCnt{CardinalityType{2}, EstimationSource::Code};
    const ScalarHistogram hist = createHistogram(data);
    const auto ceHist =
        CEHistogram::make(hist, TypeCounts{{NumberInt64, intCnt.toDouble()}}, intCnt.toDouble());

    {  // {a: {$gte: 1}}
        Interval interval(fromjson("{'': 1, '': Infinity}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // {a: {$gte: NaN}}
        Interval interval(fromjson("{'': NaN, '': NaN}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // {a: {$gte: "abc"}}
        Interval interval(fromjson("{'': \"abc\", '': {}}"), true, false);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }

    {  // The interval is unexpected from IndexBoundBuilder but still estimable like [10,
       // Infinity].
        Interval interval(fromjson("{'': 10, '':  \"\"}"), true, false);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }
}

TEST(HistogramPredicateEstimationTest, CanEstimateEmptyHistogram) {

    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;

    // Create empty histogram.
    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsBooleanOnlyFalse) {

    size_t size = 10;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < size; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(collSize,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$eq: true}}
        Interval interval(fromjson("{'': true, '': true}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(mongo::cost_based_ranker::zeroCE,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsBooleanOnlyTrue) {

    size_t size = 10;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < size; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(mongo::cost_based_ranker::zeroCE,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$eq: true}}
        Interval interval(fromjson("{'': true, '': true}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(collSize,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsBooleanMix) {

    size_t trueValues = 8, falseValues = 2, size = trueValues + falseValues;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};
    CardinalityEstimate falseCnt{CardinalityType{static_cast<double>(falseValues)},
                                 EstimationSource::Histogram};
    CardinalityEstimate trueCnt{CardinalityType{static_cast<double>(trueValues)},
                                EstimationSource::Histogram};

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(falseCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$eq: true}}
        Interval interval(fromjson("{'': true, '': true}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(trueCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsBooleanMixDifferentBounds) {

    size_t trueValues = 8, falseValues = 2, size = trueValues + falseValues;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$or: {{$eq: false}, {$eq: true}} }
        Interval interval(fromjson("{'': false, '': true}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(collSize,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$or: {{$eq: true}, {$eq: false}} }
        Interval interval(fromjson("{'': true, '': false}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(collSize,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsBooleanMixNotInclusiveBounds) {

    size_t trueValues = 8, falseValues = 2, size = trueValues + falseValues;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};
    CardinalityEstimate trueCnt{CardinalityType{static_cast<double>(trueValues)},
                                EstimationSource::Histogram};
    CardinalityEstimate falseCnt{CardinalityType{static_cast<double>(falseValues)},
                                 EstimationSource::Histogram};
    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$or: {{$eq: false}, {$eq: true}} }
        Interval interval(fromjson("{'': false, '': true}"), false, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(trueCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$or: {{$eq: false}, {$eq: true}} }
        Interval interval(fromjson("{'': false, '': true}"), true, false);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(falseCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$or: {{$eq: true}, {$eq: false}} }
        Interval interval(fromjson("{'': true, '': false}"), false, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(falseCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$or: {{$eq: true}, {$eq: false}} }
        Interval interval(fromjson("{'': true, '': false}"), true, false);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(trueCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsBooleanMixNotInclusiveBounds2) {

    size_t trueValues = 8, falseValues = 2, size = trueValues + falseValues;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // [false, false)
        Interval interval(fromjson("{'': false, '': false}"), true, false);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(cost_based_ranker::zeroCE,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

DEATH_TEST(HistogramPredicateEstimationTest,
           EstimateViaTypeCountsEmptyArray,
           "Hit a MONGO_UNREACHABLE_TASSERT") {

    size_t size = 10;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < size; i++) {
        data.push_back(sbe::value::makeNewArray());
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {
        // The interval could come from a find query like {a: {$eq: []}}. We are unable to estimate
        // empty array because the CEHistogram does not record the counts of nested empty arrays.
        // For example, a document matching the predicate like {a: [[]]} fails to be included in the
        // estimate.
        Interval interval(fromjson("{'': [], '': []}"), true, true);
        ASSERT_FALSE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_THROWS_CODE(
            HistogramEstimator::estimateCardinality(
                *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE),
            DBException,
            8870500);
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsNull) {

    size_t size = 10;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < size; i++) {
        data.push_back(stats::makeNullValue());
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: null}}
        Interval interval(fromjson("{'': null, '': null}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(collSize,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsNaN) {

    size_t size = 7, sizeNaN = 3, totalSize = size + sizeNaN;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(totalSize)},
                                 EstimationSource::Histogram};
    CardinalityEstimate nanCnt{CardinalityType{static_cast<double>(sizeNaN)},
                               EstimationSource::Histogram};

    std::vector<stats::SBEValue> data = {stats::makeDoubleValue(100.047),
                                         stats::makeDoubleValue(178.127),
                                         stats::makeDoubleValue(861.267),
                                         stats::makeDoubleValue(446.197),
                                         stats::makeDoubleValue(763.798),
                                         stats::makeDoubleValue(428.679),
                                         stats::makeDoubleValue(432.447)};

    // add NaN values.
    for (size_t i = 0; i < sizeNaN; i++) {
        data.push_back(stats::makeNaNValue());
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: NaN}}
        Interval interval(fromjson("{'': NaN, '': NaN}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(nanCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsAllString) {

    CardinalityEstimate size{CardinalityType{10}, EstimationSource::Histogram};
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data = {
        value::makeNewString("wc2VFWKqCZT3V8GVLWqAJ442vWYgKJIviv9pZqrrGD4Yyjk9epx9J9RflpASGi97BCS"),
        value::makeNewString("LjzZ9RmI4KsGgU8DEiEIe9VWFUicFHSyD5irCgWXUwh0kBV3ADkaOzxejDLK3FHt0Vl"),
        value::makeNewString("MZUjm9UCx5Kv97nuc3dXDul7NW8iCOTlY0MbCjeyxi18dCw"),
        value::makeNewString("fpOYzNMqdqeBvPKIDQ5LwrgeiYWdPfIWrWJTtPVn1khtHcQ5IyWeQBu8IS4gLzqGgUj"),
        value::makeNewString("eoktVgPzGp6NvUYZPAAy0uYv342tXltHYqX4oAxwIB1DnLPO4C3DqmhyuvKdPHxjVpM"),
        value::makeNewString("IO8ycvxdMyRveS4hMdej2O8FN2WipSbvi116Sdf97hAM4VtrGOMMqxpBwqIY5szeZC1"),
        value::makeNewString("GPqhYMa7tcl0pp5cmQqpbEt11dZjXKkxwNaZE0TOSxQeLk6xSmDY2PDfZ0XFeLlCZmH"),
        value::makeNewString("kEqBJ7aCd0ROzP6ScOiWm4xWVWPwwTvXtv7119VdSOAtiZKlmTqXvOoJvKJAnEAqrdd"),
        value::makeNewString("OsNrN0e2BxnRA8mwTQKGtgXx8GbJZmvDH38RJJywp614ff36UFfPttEuAUj1oaIM5vg"),
        value::makeNewString("rPfTNYop7sT4hUnkkg4VBKoWLlD1vJxpVWKLOx4uoJPphSU7MeOFWNU7MMksJiua4Q")};

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {$and: [{a: {$gte: ""}},{a: {$lt: {}}}]}
        Interval interval(fromjson("{'': \"\", '': {}}"), true, false);

        stats::SBEValue start = sbe::bson::convertFrom<false>(interval.start);
        stats::SBEValue end = sbe::bson::convertFrom<false>(interval.end);

        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(size,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, size, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateViaTypeCountsMixTypes) {

    size_t strCount = 10, doubleCount = 7, sizeNaN = 3, trueValues = 5, falseValues = 5,
           size = strCount + doubleCount + sizeNaN + trueValues + falseValues;
    size_t numberOfBuckets = 10;
    CardinalityEstimate collSize{CardinalityType{static_cast<double>(size)},
                                 EstimationSource::Histogram};
    CardinalityEstimate trueCnt{CardinalityType{static_cast<double>(trueValues)},
                                EstimationSource::Histogram};
    CardinalityEstimate falseCnt{CardinalityType{static_cast<double>(falseValues)},
                                 EstimationSource::Histogram};
    CardinalityEstimate nanCnt{CardinalityType{static_cast<double>(sizeNaN)},
                               EstimationSource::Histogram};
    CardinalityEstimate strCnt{CardinalityType{static_cast<double>(strCount)},
                               EstimationSource::Histogram};

    std::vector<stats::SBEValue> data = {
        value::makeNewString("wc2VFWKqCZT3V8GVLWqAJ442vWYgKJIviv9pZqrrGD4Yyjk9epx9J9RflpASGi97BCS"),
        value::makeNewString("LjzZ9RmI4KsGgU8DEiEIe9VWFUicFHSyD5irCgWXUwh0kBV3ADkaOzxejDLK3FHt0Vl"),
        value::makeNewString("MZUjm9UCx5Kv97nuc3dXDul7NW8iCOTlY0MbCjeyxi18dCw"),
        value::makeNewString("fpOYzNMqdqeBvPKIDQ5LwrgeiYWdPfIWrWJTtPVn1khtHcQ5IyWeQBu8IS4gLzqGgUj"),
        value::makeNewString("eoktVgPzGp6NvUYZPAAy0uYv342tXltHYqX4oAxwIB1DnLPO4C3DqmhyuvKdPHxjVpM"),
        value::makeNewString("IO8ycvxdMyRveS4hMdej2O8FN2WipSbvi116Sdf97hAM4VtrGOMMqxpBwqIY5szeZC1"),
        value::makeNewString("GPqhYMa7tcl0pp5cmQqpbEt11dZjXKkxwNaZE0TOSxQeLk6xSmDY2PDfZ0XFeLlCZmH"),
        value::makeNewString("kEqBJ7aCd0ROzP6ScOiWm4xWVWPwwTvXtv7119VdSOAtiZKlmTqXvOoJvKJAnEAqrdd"),
        value::makeNewString("OsNrN0e2BxnRA8mwTQKGtgXx8GbJZmvDH38RJJywp614ff36UFfPttEuAUj1oaIM5vg"),
        value::makeNewString("rPfTNYop7sT4hUnkkg4VBKoWLlD1vJxpVWKLOx4uoJPphSU7MeOFWNU7MMksJiua4Q"),
        stats::makeDoubleValue(100.047),
        stats::makeDoubleValue(178.127),
        stats::makeDoubleValue(861.267),
        stats::makeDoubleValue(446.197),
        stats::makeDoubleValue(763.798),
        stats::makeDoubleValue(428.679),
        stats::makeDoubleValue(432.447)};

    // add True boolean values.
    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    // add True boolean values.
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    // add NaN values.
    for (size_t i = 0; i < sizeNaN; i++) {
        data.push_back(stats::makeNaNValue());
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: true}}
        Interval interval(fromjson("{'': true, '': true}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(trueCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(falseCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$eq: NaN}}
        Interval interval(fromjson("{'': NaN, '': NaN}"), true, true);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(nanCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {$and: [{a: {$gte: ""}},{a: {$lt: {}}}]}
        Interval interval(fromjson("{'': \"\", '': {}}"), true, false);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));

        ASSERT_EQ(strCnt,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, collSize, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, EstimateEmptyHistogram) {

    CardinalityEstimate size{CardinalityType{10}, EstimationSource::Histogram};
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;

    // Create empty histogram.
    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        ASSERT_EQ(mongo::cost_based_ranker::zeroCE,
                  HistogramEstimator::estimateCardinality(
                      *ceHist, size, interval, true, ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, IntHistogramIntervalEstimation) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 1.0, 10.0, 5.0},
                                 {20, 3.0, 15.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const CardinalityEstimate intCnt{CardinalityType{55}, EstimationSource::Code};
    const ScalarHistogram hist = createHistogram(data);
    const auto ceHist =
        CEHistogram::make(hist, TypeCounts{{NumberInt64, intCnt.toDouble()}}, intCnt.toDouble());

    {  // {a: 20}
        Interval interval(BSON("" << 20 << "" << 20), true /*startIncluded*/, true
                          /*endIncluded*/);
        auto estimatedCard =
            estimateCardinalityEq(*ceHist, NumberInt64, 20, true /*includeScalar*/).card;
        ASSERT_EQ(3.0, estimatedCard);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /* includeScalar */,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // {a: {$gte: 20, $lte: 30}}
        Interval interval(BSON("" << 20 << "" << 30), true, true);
        auto estimatedCard = estimateCardinalityRange(*ceHist,
                                                      true /*lowInclusive*/,
                                                      NumberInt64,
                                                      20,
                                                      true /*highInclusive*/,
                                                      NumberInt64,
                                                      30,
                                                      true /*includeScalar*/,
                                                      ArrayRangeEstimationAlgo::kConjunctArrayCE)
                                 .card;
        ASSERT_EQ(14.0, estimatedCard);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  //  {a: {$gte: 20, $lte: 25}}, bucket interpolation.
        Interval interval(BSON("" << 20 << "" << 25), true /*startIncluded*/, true
                          /*endIncluded*/);
        auto estimatedCard = estimateCardinalityRange(*ceHist,
                                                      true /*lowInclusive*/,
                                                      NumberInt64,
                                                      20,
                                                      true /*highInclusive*/,
                                                      NumberInt64,
                                                      25,
                                                      true /*includeScalar*/,
                                                      ArrayRangeEstimationAlgo::kConjunctArrayCE)
                                 .card;
        ASSERT_EQ(8.0, estimatedCard);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // {a: {$gte: 30, $lte: 40}}
        Interval interval(BSON("" << 30 << "" << 40), true /*startIncluded*/, true
                          /*endIncluded*/);
        ASSERT_EQ(3.0,
                  HistogramEstimator::estimateCardinality(*ceHist,
                                                          intCnt,
                                                          interval,
                                                          true /*includeScalar*/,
                                                          ArrayRangeEstimationAlgo::kExactArrayCE)
                      .toDouble());
    }

    {  // {a: {$gte: 30}}
        Interval interval(BSON("" << 30 << "" << std::numeric_limits<double>::infinity()),
                          true /*startIncluded*/,
                          true /*endIncluded*/);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(14.0), EstimationSource::Code),
                  HistogramEstimator::estimateCardinality(*ceHist,
                                                          intCnt,
                                                          interval,
                                                          true /*includeScalar*/,
                                                          ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // Interval [30, "") is supposed to have the same estimate as [30, Infinity].
        Interval interval(BSON("" << 30 << ""
                                  << ""),
                          true /*startIncluded*/,
                          false /*endIncluded*/);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(14.0), EstimationSource::Code),
                  HistogramEstimator::estimateCardinality(*ceHist,
                                                          intCnt,
                                                          interval,
                                                          true /*includeScalar*/,
                                                          ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {a: {$gte: -Infinity}}
        Interval interval(BSON("" << -std::numeric_limits<double>::infinity() << ""
                                  << std::numeric_limits<double>::infinity()),
                          true /*startIncluded*/,
                          true /*endIncluded*/);
        ASSERT_EQ(54.5,
                  HistogramEstimator::estimateCardinality(*ceHist,
                                                          intCnt,
                                                          interval,
                                                          true /*includeScalar*/,
                                                          ArrayRangeEstimationAlgo::kExactArrayCE)
                      .toDouble());
    }
}

TEST(HistogramPredicateEstimationTest, StrHistogramIntervalEstimation) {
    std::vector<BucketData> data{{
        {"0ejz", 1, 0, 0},
        {"4FGjc", 3, 5, 3},
        {"9bU3", 2, 3, 2},
        {"Cy5Kw", 3, 3, 3},
        {"Lm4U", 2, 11, 5},
        {"TTV", 5, 14, 8},
        {"YtzS", 2, 3, 2},
        {"o9cD4", 6, 26, 16},
        {"qfmnP", 1, 4, 2},
        {"xqbi", 2, 4, 4},
    }};
    const CardinalityEstimate strCnt{CardinalityType{100}, EstimationSource::Code};
    const ScalarHistogram& hist = createHistogram(data);

    const auto ceHist = CEHistogram::make(
        hist, stats::TypeCounts{{StringSmall, strCnt.toDouble()}}, strCnt.toDouble());

    auto [tagLow, valLow] = value::makeNewString("TTV"_sd);
    value::ValueGuard vgLow(tagLow, valLow);

    {  // {a: "TTV"}
        Interval interval(BSON("" << "TTV"
                                  << ""
                                  << "TTV"),
                          true,
                          true);
        auto estimatedCard =
            estimateCardinalityEq(*ceHist, tagLow, valLow, true /*includeScalar*/).card;
        ASSERT_EQ(5.0, estimatedCard);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  (estimateIntervalCardinality(*ceHist,
                                               interval,
                                               true /*includeScalar*/,
                                               ArrayRangeEstimationAlgo::kConjunctArrayCE)));
    }

    {  // {a: {$gte: "TTV", $lte: "YtzS"}}
        auto [tagHigh, valHigh] = value::makeNewString("YtzS"_sd);
        value::ValueGuard vgHigh(tagHigh, valHigh);
        Interval interval(BSON("" << "TTV"
                                  << ""
                                  << "YtzS"),
                          true,
                          true);
        auto estimatedCard = estimateCardinalityRange(*ceHist,
                                                      true /*lowInclusive*/,
                                                      tagLow,
                                                      valLow,
                                                      true /*highInclusive*/,
                                                      tagHigh,
                                                      valHigh,
                                                      true /*includeScalar*/,
                                                      ArrayRangeEstimationAlgo::kConjunctArrayCE)
                                 .card;
        ASSERT_EQ(10.0, estimatedCard);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // {a: {$gte: "TTV", $lte: "VtzSlajdkajda"}} (tests for memory leaks for a large string)
        auto [tagHigh, valHigh] = value::makeNewString("VtzSlajdkajda"_sd);
        value::ValueGuard vgHigh(tagHigh, valHigh);
        Interval interval(BSON("" << "TTV"
                                  << ""
                                  << "VtzSlajdkajda"),
                          true,
                          true);
        auto estimatedCard = estimateCardinalityRange(*ceHist,
                                                      true /*lowInclusive*/,
                                                      tagLow,
                                                      valLow,
                                                      true /*highInclusive*/,
                                                      tagHigh,
                                                      valHigh,
                                                      true /*includeScalar*/,
                                                      ArrayRangeEstimationAlgo::kConjunctArrayCE)
                                 .card;
        ASSERT_APPROX_EQUAL(6.244, estimatedCard, 0.001);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // {a: {$gte: "YtzS", $lte: "o9cD4"}}
        Interval interval(BSON("" << "YtzS"
                                  << ""
                                  << "o9cD4"),
                          true,
                          true);
        ASSERT_EQ(34.0,
                  HistogramEstimator::estimateCardinality(*ceHist,
                                                          strCnt,
                                                          interval,
                                                          true /*includeScalar*/,
                                                          ArrayRangeEstimationAlgo::kExactArrayCE)
                      .toDouble());
    }

    {  // {a: {$gte: "YtzS"}}
        Interval interval(BSON("" << "YtzS"
                                  << "" << BSONObj()),
                          true,
                          false);
        ASSERT_EQ(45.0,
                  HistogramEstimator::estimateCardinality(*ceHist,
                                                          strCnt,
                                                          interval,
                                                          true /*includeScalar*/,
                                                          ArrayRangeEstimationAlgo::kExactArrayCE)
                      .toDouble());
    }
}

TEST(HistogramPredicateEstimationTest, IntStrHistogramIntervalEstimation) {
    std::vector<BucketData> data{{
        {2, 3, 0, 0},       {19, 4, 1, 1},      {226, 2, 49, 20},  {301, 5, 12, 4},
        {317, 3, 0, 0},     {344, 2, 3, 1},     {423, 5, 18, 6},   {445, 3, 0, 0},
        {495, 3, 4, 2},     {542, 5, 9, 3},     {696, 3, 44, 19},  {773, 4, 11, 5},
        {805, 2, 8, 4},     {931, 5, 21, 8},    {998, 4, 21, 3},   {"8N4", 5, 31, 14},
        {"MIb", 5, 45, 17}, {"Zgi", 3, 55, 22}, {"pZ", 6, 62, 25}, {"yUwxz", 5, 29, 12},
    }};
    const CardinalityEstimate intCnt{CardinalityType{254}, EstimationSource::Code};
    const CardinalityEstimate strCnt{CardinalityType{246}, EstimationSource::Code};
    const CardinalityEstimate totalCnt{CardinalityType{(intCnt + strCnt).toDouble()},
                                       EstimationSource::Code};
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(totalCnt.toDouble(), getTotals(hist).card);

    const auto ceHist = CEHistogram::make(
        hist,
        stats::TypeCounts{{NumberInt64, intCnt.toDouble()}, {StringSmall, strCnt.toDouble()}},
        totalCnt.toDouble());

    {  // {a: 993}
        Interval interval(BSON("" << 993 << "" << 993), true, true);
        auto estimatedCard =
            estimateCardinalityEq(*ceHist, NumberInt64, 993, true /*includeScalar*/).card;
        ASSERT_APPROX_EQUAL(7.0, estimatedCard,
                            0.1);  // Actual: 9
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // {a: "04e"}
        auto [tag, value] = sbe::value::makeNewString("04e"_sd);
        sbe::value::ValueGuard vg(tag, value);
        Interval interval(BSON("" << "04e"
                                  << ""
                                  << "04e"),
                          true /*startIncluded*/,
                          true /*endIncluded*/);
        auto estimatedCard =
            estimateCardinalityEq(*ceHist, tag, value, true /*includeScalar*/).card;
        ASSERT_APPROX_EQUAL(2.2, estimatedCard, 0.1);  // Actual: 3.
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // {a: 100000000}
        value::TypeTags tagLow = NumberInt64;
        value::Value valLow = 100000000;
        Interval interval(BSON("" << 100000000 << "" << 100000000), true /*startIncluded*/, true
                          /*endIncluded*/);
        auto estimatedCard =
            estimateCardinalityEq(*ceHist, tagLow, valLow, true /*includeScalar*/).card;
        ASSERT_APPROX_EQUAL(0.0, estimatedCard,
                            0.1);  // Actual: 0
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // {$match: {a: {$lt: '04e'}}}
        auto [tagLow, valLow] = sbe::value::makeNewString(""_sd);
        auto [tagHigh, valHigh] = sbe::value::makeNewString("04e"_sd);
        sbe::value::ValueGuard vgLow(tagLow, valLow);
        sbe::value::ValueGuard vgHigh(tagHigh, valHigh);
        Interval interval(BSON("" << ""
                                  << ""
                                  << "04e"),
                          true,
                          false);
        auto estimatedCard = estimateCardinalityRange(*ceHist,
                                                      false /* lowInclusive */,
                                                      tagLow,
                                                      valLow,
                                                      false /* highInclusive */,
                                                      tagHigh,
                                                      valHigh,
                                                      true /* includeScalar */,
                                                      ArrayRangeEstimationAlgo::kConjunctArrayCE)
                                 .card;
        ASSERT_APPROX_EQUAL(13.3, estimatedCard, 0.1);  // Actual: 0.
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, IntArrayOnlyIntervalEstimate) {
    // This hard-codes a maxdiff histogram with 10 buckets built off of an array distribution
    // with arrays between 3 and 5 elements long, each containing 100 distinct ints uniformly
    // distributed between 0 and 1000. There are no scalar elements.
    std::vector<BucketData> scalarData{{}};
    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{{
        {5, 3, 0, 0},   {19, 5, 2, 1},  {57, 4, 4, 3},  {116, 7, 13, 7}, {198, 3, 15, 6},
        {228, 2, 3, 2}, {254, 4, 0, 0}, {280, 2, 2, 1}, {335, 3, 5, 3},  {344, 2, 0, 0},
        {388, 3, 0, 0}, {420, 2, 0, 0}, {454, 1, 6, 3}, {488, 2, 1, 1},  {530, 1, 0, 0},
        {561, 1, 0, 0}, {609, 1, 0, 0}, {685, 1, 0, 0}, {713, 1, 0, 0},  {758, 1, 0, 0},
    }};
    const ScalarHistogram minHist = createHistogram(minData);

    std::vector<BucketData> maxData{{
        {301, 1, 0, 0},  {408, 2, 0, 0}, {445, 1, 0, 0}, {605, 2, 0, 0},  {620, 1, 0, 0},
        {665, 1, 1, 1},  {687, 3, 0, 0}, {704, 2, 6, 2}, {718, 2, 2, 1},  {741, 2, 1, 1},
        {752, 2, 0, 0},  {823, 7, 3, 3}, {827, 1, 0, 0}, {852, 3, 0, 0},  {864, 5, 0, 0},
        {909, 7, 12, 5}, {931, 2, 3, 1}, {939, 3, 0, 0}, {970, 2, 12, 4}, {998, 1, 10, 4},
    }};
    const ScalarHistogram maxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{{
        {5, 3, 0, 0},    {19, 6, 2, 1},    {57, 4, 4, 3},    {116, 7, 15, 8},  {228, 2, 38, 13},
        {254, 7, 0, 0},  {269, 10, 0, 0},  {280, 7, 3, 1},   {306, 4, 1, 1},   {317, 4, 0, 0},
        {344, 2, 19, 5}, {423, 2, 27, 8},  {507, 2, 22, 13}, {704, 8, 72, 34}, {718, 6, 3, 1},
        {758, 3, 13, 4}, {864, 7, 35, 14}, {883, 4, 0, 0},   {939, 5, 32, 10}, {998, 1, 24, 9},
    }};

    const CardinalityEstimate totalCnt{CardinalityType{100.0}, EstimationSource::Code};

    const ScalarHistogram uniqueHist = createHistogram(uniqueData);

    const auto ceHist = CEHistogram::make(scalarHist,
                                          TypeCounts{{value::TypeTags::Array, 100}},
                                          uniqueHist,
                                          minHist,
                                          maxHist,
                                          // There are 100 non-empty int-only arrays.
                                          TypeCounts{{value::TypeTags::NumberInt64, 100}},
                                          totalCnt.toDouble() /* sampleSize */);

    {  // {$match: {a: {$elemMatch: {$gt: 500, $lt: 600}}}}
        value::TypeTags tagLow = NumberInt64;
        value::Value valLow = 500;
        value::TypeTags tagHigh = NumberInt64;
        value::Value valHigh = 600;
        Interval interval(BSON("" << 500 << "" << 600), false, false /*endIncluded*/);
        auto estimatedCard = estimateCardinalityRange(*ceHist,
                                                      false /*lowInclusive*/,
                                                      tagLow,
                                                      valLow,
                                                      false /*highInclusive*/,
                                                      tagHigh,
                                                      valHigh,
                                                      false /*includeScalar*/,
                                                      ArrayRangeEstimationAlgo::kExactArrayCE)
                                 .card;
        ASSERT_APPROX_EQUAL(27.0, estimatedCard, 0.1);  // actual 21.
        ASSERT_EQ(CardinalityEstimate(CardinalityType(estimatedCard), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              false /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kExactArrayCE));
    }

    {  // {$match: {a: {$elemMatch: {$gt: 10, $lt: 110}}}
        Interval interval(
            BSON("" << 10 << "" << 110), false /*startIncluded*/, false /*endIncluded*/);
        ASSERT_EQ(CardinalityEstimate(CardinalityType(24.1065), EstimationSource::Code),
                  HistogramEstimator::estimateCardinality(*ceHist,
                                                          totalCnt,
                                                          interval,
                                                          false /*includeScalar*/,
                                                          ArrayRangeEstimationAlgo::kExactArrayCE));
    }
}

TEST(HistogramPredicateEstimationTest, NonHistogrammableTypesEstimation) {
    const int64_t startInstant = 1496777923LL;
    const int64_t endInstant = 1496864323LL;
    const Timestamp& startTs{Seconds(startInstant), 0};
    const Timestamp& endTs{Seconds(endInstant), 0};
    std::vector<BucketData> data{{1, 10.0, 0.0, 0.0},
                                 {10, 20.0, 0.0, 0.0},
                                 {Value(startTs), 20.0, 0.0, 0.0},
                                 {Value(endTs), 5.0, 0.0, 0.0}};
    const CardinalityEstimate totalCnt{CardinalityType{100.0}, EstimationSource::Code};
    const ScalarHistogram& hist = createHistogram(data);

    const auto ceHist = CEHistogram::make(hist,
                                          stats::TypeCounts{{value::TypeTags::NumberInt64, 30},
                                                            {value::TypeTags::Timestamp, 25},
                                                            {value::TypeTags::Boolean, 25},
                                                            {value::TypeTags::Null, 5},
                                                            {value::TypeTags::Object, 15}},
                                          totalCnt.toDouble(),
                                          5,
                                          20);

    {  // check estimation for Boolean
        Interval interval(
            BSON("" << true << "" << true), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(
            CardinalityEstimate(CardinalityType(5), EstimationSource::Code), /*estimatedCard */
            estimateIntervalCardinality(*ceHist,
                                        interval,
                                        true /*includeScalar*/,
                                        ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // check estimation for Null
        Interval interval(BSON("" << BSONNULL << "" << BSONNULL), true /*startIncluded*/, true
                          /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(CardinalityEstimate(CardinalityType(5), EstimationSource::Code), /*estimatedCard*/
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // check estimation for Timestamp
        Interval interval(
            BSON("" << startTs << "" << endTs), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(
            CardinalityEstimate(CardinalityType(25), EstimationSource::Code), /*estimatedCard*/
            estimateIntervalCardinality(*ceHist,
                                        interval,
                                        true /*includeScalar*/,
                                        ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // check estimation for [Null, true]
        Interval interval(
            BSON("" << BSONNULL << "" << true), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(
            CardinalityEstimate(CardinalityType(75), EstimationSource::Code), /*estimatedCard ,*/
            estimateIntervalCardinality(*ceHist,
                                        interval,
                                        true /*includeScalar*/,
                                        ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }

    {  // check estimation for [false, timestamp]
        Interval interval(
            BSON("" << false << "" << endTs), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(
            CardinalityEstimate(CardinalityType(50), EstimationSource::Code), /*estimatedCard ,*/
            estimateIntervalCardinality(*ceHist,
                                        interval,
                                        true /*includeScalar*/,
                                        ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }
}

DEATH_TEST(HistogramPredicateEstimationTest,
           NonEstimableTypesEstimation,
           "Hit a MONGO_UNREACHABLE_TASSERT") {
    const int64_t startInstant = 1496777923LL;
    const int64_t endInstant = 1496864323LL;
    const Timestamp& startTs{Seconds(startInstant), 0};
    const Timestamp& endTs{Seconds(endInstant), 0};
    std::vector<BucketData> data{{1, 10.0, 0.0, 0.0},
                                 {10, 20.0, 0.0, 0.0},
                                 {Value(startTs), 20.0, 0.0, 0.0},
                                 {Value(endTs), 5.0, 0.0, 0.0}};
    const CardinalityEstimate totalCnt{CardinalityType{100.0}, EstimationSource::Code};
    const ScalarHistogram& hist = createHistogram(data);

    const auto ceHist = CEHistogram::make(hist,
                                          stats::TypeCounts{{value::TypeTags::NumberInt64, 30},
                                                            {value::TypeTags::Timestamp, 25},
                                                            {value::TypeTags::Boolean, 25},
                                                            {value::TypeTags::Null, 5},
                                                            {value::TypeTags::Object, 15}},
                                          totalCnt.toDouble(),
                                          5,
                                          20);
    {  // check estimation for Object (expected to fail)
        Interval interval(BSON("" << BSON("" << "") << "" << BSON("" << "")),
                          true /*startIncluded*/,
                          true /*endIncluded*/);
        ASSERT_FALSE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_THROWS_CODE(estimateIntervalCardinality(*ceHist,
                                                       interval,
                                                       true /*includeScalar*/,
                                                       ArrayRangeEstimationAlgo::kConjunctArrayCE),
                           DBException,
                           8870500);
    }
}

TEST(HistogramPredicateEstimationTest, MixedTypeIntervalEstimation) {
    const int64_t startInstant = 1496777923LL;
    const int64_t endInstant = 1496864323LL;
    const Timestamp& startTs{Seconds(startInstant), 0};
    const Timestamp& endTs{Seconds(endInstant), 0};
    std::vector<BucketData> data{{1, 10.0, 0.0, 0.0},
                                 {10, 20.0, 0.0, 0.0},
                                 {Value(startTs), 20.0, 0.0, 0.0},
                                 {Value(endTs), 5.0, 0.0, 0.0}};
    const CardinalityEstimate totalCnt{CardinalityType{100.0}, EstimationSource::Code};
    const ScalarHistogram& hist = createHistogram(data);
    const auto ceHist = CEHistogram::make(hist,
                                          stats::TypeCounts{
                                              {value::TypeTags::MinKey, 1},
                                              {value::TypeTags::NumberInt64, 30},
                                              {value::TypeTags::Timestamp, 25},
                                              {value::TypeTags::Boolean, 25},
                                              {value::TypeTags::Null, 5},
                                              {value::TypeTags::Object, 12},
                                              {value::TypeTags::MaxKey, 2},
                                          },
                                          totalCnt.toDouble(),
                                          5,
                                          20);

    {  // [MinKey, MaxKey]
        Interval interval(
            BSON("" << MINKEY << "" << MAXKEY), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(CardinalityEstimate(CardinalityType(100), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }
    {  // [MinKey, endTs]
        Interval interval(
            BSON("" << MINKEY << "" << endTs), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(CardinalityEstimate(CardinalityType(98), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }
    {  // [0.0, MaxKey]
        Interval interval(
            BSON("" << 0.0 << "" << MAXKEY), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(CardinalityEstimate(CardinalityType(94), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }
    {  // [0.0, endTs]
        Interval interval(
            BSON("" << 0.0 << "" << endTs), true /*startIncluded*/, true /*endIncluded*/);
        ASSERT_TRUE(HistogramEstimator::canEstimateInterval(*ceHist, interval));
        ASSERT_EQ(CardinalityEstimate(CardinalityType(92), EstimationSource::Code),
                  estimateIntervalCardinality(*ceHist,
                                              interval,
                                              true /*includeScalar*/,
                                              ArrayRangeEstimationAlgo::kConjunctArrayCE));
    }
}

}  // namespace
}  // namespace mongo::ce
