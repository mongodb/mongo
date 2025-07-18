/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/query/compiler/stats/ce_histogram.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/max_diff.h"
#include "mongo/db/query/compiler/stats/rand_utils_new.h"
#include "mongo/db/query/compiler/stats/scalar_histogram.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace mongo::stats {

TEST(CEHistograms, BSONEdgeValues) {
    const std::vector<SBEValue> values{
        SBEValue{sbe::value::TypeTags::Nothing, {}},

        makeInt32Value(std::numeric_limits<int32_t>::min()),
        makeInt32Value(std::numeric_limits<int32_t>::max()),

        makeInt64Value(std::numeric_limits<int>::min()),
        makeInt64Value(std::numeric_limits<int>::max()),

        makeDoubleValue(std::numeric_limits<double>::lowest()),
        makeDoubleValue(std::numeric_limits<double>::min()),
        makeDoubleValue(std::numeric_limits<double>::max()),
        // TODO SERVER-73510: Support infinity in histograms
        // makeDoubleValue(std::numeric_limits<double>::infinity()),
        makeDoubleValue(std::numeric_limits<double>::quiet_NaN()),
        makeDoubleValue(std::numeric_limits<double>::signaling_NaN()),

        makeDateValue(Date_t::min()),
        makeDateValue(Date_t::max()),

        makeTimestampValue(Timestamp::min()),
        makeTimestampValue(Timestamp::max()),

        SBEValue{sbe::value::TypeTags::Boolean, true},
        SBEValue{sbe::value::TypeTags::Boolean, false},

        makeNullValue(),

        sbe::value::makeNewString(""),
        sbe::value::makeNewString("a"),
        sbe::value::makeNewString("aaaaaaaaaaaaaaaaaa"),  // Force heap-allocated string
        sbe::value::makeNewString(std::string(10000, 'a')),
        // TODO SERVER-72850: Support strings with Unicode characters in histograms
        // sbe::value::makeNewString("😊"),

        SBEValue{sbe::value::TypeTags::MinKey, {}},
        SBEValue{sbe::value::TypeTags::MaxKey, {}},

        sbe::value::makeCopyDecimal(Decimal128::kLargestNegative),
        sbe::value::makeCopyDecimal(Decimal128::kSmallestNegative),
        sbe::value::makeCopyDecimal(Decimal128::kSmallestPositive),
        sbe::value::makeCopyDecimal(Decimal128::kLargestPositive),
        sbe::value::makeCopyDecimal(Decimal128::kNormalizedZero),
        sbe::value::makeCopyDecimal(Decimal128::kLargestNegativeExponentZero),
        // TODO SERVER-73510: Support infinity in histograms
        // sbe::value::makeCopyDecimal(Decimal128::kNegativeInfinity),
        // sbe::value::makeCopyDecimal(Decimal128::kPositiveInfinity),
        sbe::value::makeCopyDecimal(Decimal128::kNegativeNaN),
        sbe::value::makeCopyDecimal(Decimal128::kPositiveNaN),

        sbe::value::makeNewArray(),
        sbe::value::makeCopyArray([] {
            sbe::value::Array largeArray;
            largeArray.reserve(10000);
            for (int i = 0; i < 10000; ++i) {
                largeArray.push_back(sbe::value::TypeTags::NumberInt64, i);
            }
            return largeArray;
        }()),
        sbe::value::makeNewObject(),
        sbe::value::makeNewObjectId(),
    };
    auto ceHist = createCEHistogram(values, ScalarHistogram::kMaxBuckets);
    // We are relying on the fact that 'createCEHistogram()' performs validation of the histogram
    // upon construction.

    TypeCounts expectedTypeCounts = {
        {sbe::value::TypeTags::Nothing, 1},
        {sbe::value::TypeTags::NumberInt32, 2},
        {sbe::value::TypeTags::NumberInt64, 2},
        {sbe::value::TypeTags::NumberDouble, 5},
        {sbe::value::TypeTags::Date, 2},
        {sbe::value::TypeTags::Timestamp, 2},
        {sbe::value::TypeTags::Boolean, 2},
        {sbe::value::TypeTags::Null, 1},
        {sbe::value::TypeTags::StringSmall, 2},
        {sbe::value::TypeTags::StringBig, 2},
        {sbe::value::TypeTags::MinKey, 1},
        {sbe::value::TypeTags::MaxKey, 1},
        {sbe::value::TypeTags::NumberDecimal, 8},
        {sbe::value::TypeTags::Array, 2},
        {sbe::value::TypeTags::Object, 1},
        {sbe::value::TypeTags::ObjectId, 1},
    };
    ASSERT_EQ(expectedTypeCounts, ceHist->getTypeCounts());
    ASSERT_EQ(ceHist->getTrueCount(), 1);
    ASSERT_EQ(ceHist->getFalseCount(), 1);
    ASSERT_EQ(ceHist->getNanCount(), 4);
    ASSERT_EQ(ceHist->getEmptyArrayCount(), 1);

    // Verify that we can build a histogram with the number of buckets equal to the number of
    // types in the value stream + 1 (numeric, date, timestamp, string, and objectId).
    ceHist = createCEHistogram(values, 6);

    // Ensure we fail to build a histrogram when we have more types than buckets.
    ASSERT_THROWS(createCEHistogram(values, 5), DBException);
}

TEST(CEHistograms, EmptyHistogram) {
    auto ceHist = createCEHistogram({}, ScalarHistogram::kMaxBuckets);
}

TEST(CEHistograms, SingleEntryHistogram) {
    const Date_t d = dateFromISOString("2015-10-21T07:28:00+0000").getValue();
    const std::vector<SBEValue> values{
        makeInt64Value(42),
        makeDoubleValue(42.0),
        makeDateValue(d),
        makeTimestampValue(Timestamp(d)),
        sbe::value::makeNewString("mcfly"),
        sbe::value::makeNewObjectId(),
    };
    for (auto&& v : values) {
        std::vector<SBEValue> singleValVec{sbe::value::copyValue(v.getTag(), v.getValue())};
        auto ceHist = createCEHistogram(singleValVec, ScalarHistogram::kMaxBuckets);
        ceHist = createCEHistogram(singleValVec, 2);
    }
}

TEST(CEHistograms, DuplicateValues) {
    auto ceHist = createCEHistogram(
        {
            makeInt64Value(1),
            makeInt64Value(1),
            makeInt64Value(2),
            sbe::value::makeNewString("marty"),
            sbe::value::makeNewString("marty"),
            sbe::value::makeNewString("mcfly"),
        },
        3);
}

TEST(CEHistograms, SingleEntryInTypeClass) {
    // Single entry at the end
    auto ceHist = createCEHistogram(
        {
            makeInt64Value(1),
            makeInt64Value(2),
            sbe::value::makeNewString("mcfly"),
        },
        3);
    // Single entry at the beginning
    ceHist = createCEHistogram(
        {
            makeInt64Value(1),
            sbe::value::makeNewString("marty"),
            sbe::value::makeNewString("mcfly"),
        },
        3);
    // Single entry in the middle
    ceHist = createCEHistogram(
        {
            makeInt64Value(1),
            makeInt64Value(2),
            sbe::value::makeNewString("mcfly"),
            makeDateValue(dateFromISOString("1985-10-26T09:00:00+0000").getValue()),
            makeDateValue(dateFromISOString("2015-10-21T07:28:00+0000").getValue()),
        },
        4);
}

TEST(CEHistograms, LargeAreasWithinTypeClass) {
    auto ceHist = createCEHistogram(
        {
            makeInt32Value(std::numeric_limits<int32_t>::min()),
            makeInt32Value(std::numeric_limits<int32_t>::max()),
            makeInt64Value(std::numeric_limits<int>::min()),
            makeInt64Value(std::numeric_limits<int>::max()),
            makeDoubleValue(std::numeric_limits<double>::lowest()),
            makeDoubleValue(std::numeric_limits<double>::max()),
            sbe::value::makeCopyDecimal(Decimal128::kLargestNegative),
            sbe::value::makeCopyDecimal(Decimal128::kLargestPositive),
        },
        2);
}

TEST(CEHistograms, SmallAreasWithinTypeClass) {
    auto ceHist = createCEHistogram(
        {
            sbe::value::makeCopyDecimal(Decimal128::kSmallestNegative),
            sbe::value::makeCopyDecimal(Decimal128::kNormalizedZero),
            sbe::value::makeCopyDecimal(Decimal128::kSmallestPositive),
        },
        2);
}

TEST(CEHistograms, MixedTypedHistrogram) {
    std::mt19937_64 seed(42);
    MixedDistributionDescriptor uniform{{DistrType::kUniform, 1.0}};
    TypeDistrVector td;
    td.push_back(std::make_unique<IntDistribution>(uniform, 0.2, 100, -100, 100));
    td.push_back(std::make_unique<StrDistribution>(uniform, 0.2, 100, 0, 20));
    td.push_back(std::make_unique<DateDistribution>(
        uniform,
        0.2,
        100,
        dateFromISOString("1985-10-26T09:00:00+0000").getValue(),
        dateFromISOString("2015-10-21T07:28:00+0000").getValue()));
    td.push_back(std::make_unique<DoubleDistribution>(uniform, 0.2, 100, -100, 100));
    td.push_back(std::make_unique<ObjectIdDistribution>(uniform, 0.2, 100));
    DatasetDescriptorNew desc{std::move(td), seed};
    const std::vector<SBEValue> values = desc.genRandomDataset(10'000);
    ASSERT_EQ(10'000, values.size());
    auto ceHist = createCEHistogram(values, ScalarHistogram::kMaxBuckets);
    ASSERT_EQ(10'000, ceHist->getScalar().getCardinality());
}

TEST(CEHistograms, LargeNumberOfScalarValuesBucketRanges) {
    std::mt19937_64 seed(42);
    MixedDistributionDescriptor uniform{{DistrType::kUniform, 1.0}};
    TypeDistrVector td;
    td.push_back(std::make_unique<IntDistribution>(uniform, 0.5, 1'000'000, 0, 1'000'000));
    DatasetDescriptorNew desc{std::move(td), seed};
    const std::vector<SBEValue> values = desc.genRandomDataset(1'000'000);
    ASSERT_EQ(1'000'000, values.size());
    auto ceHist = createCEHistogram(values, ScalarHistogram::kMaxBuckets);

    ASSERT_EQ(1'000'000, ceHist->getScalar().getCardinality());
    // Assert that each bucket has at least one entry.
    std::for_each(ceHist->getScalar().getBuckets().begin(),
                  ceHist->getScalar().getBuckets().end(),
                  [](auto&& bucket) { ASSERT_GTE(bucket._equalFreq + bucket._rangeFreq, 1); });
}

TEST(CEHistograms, LargeArraysHistogram) {
    std::mt19937_64 seed(42);
    MixedDistributionDescriptor uniform{{DistrType::kUniform, 1.0}};

    TypeDistrVector arrayData;
    arrayData.push_back(std::make_unique<IntDistribution>(uniform, 1.0, 1'000'000, 0, 1'000'000));
    auto arrayDataDesc = std::make_unique<DatasetDescriptorNew>(std::move(arrayData), seed);

    TypeDistrVector arrayDataset;
    arrayDataset.push_back(std::make_unique<ArrDistribution>(
        uniform, 1.0, 100, 80'000, 100'000, std::move(arrayDataDesc)));
    DatasetDescriptorNew arrayDatasetDesc{std::move(arrayDataset), seed};

    // Build 10 values where each value is an array of length 80-100k.
    const auto values = arrayDatasetDesc.genRandomDataset(10);
    ASSERT_EQ(10, values.size());

    auto ceHist = createCEHistogram(values, ScalarHistogram::kMaxBuckets);

    ASSERT_TRUE(ceHist->getScalar().empty());
    ASSERT_EQ(100, ceHist->getArrayUnique().getBuckets().size());
    ASSERT_FALSE(ceHist->getArrayMin().empty());
    ASSERT_FALSE(ceHist->getArrayMax().empty());
}

TEST(CEHistograms, LargeNumberOfArraysHistogram) {
    std::mt19937_64 seed(42);
    MixedDistributionDescriptor uniform{{DistrType::kUniform, 1.0}};

    TypeDistrVector arrayData;
    arrayData.push_back(std::make_unique<IntDistribution>(uniform, 1.0, 1'000'000, 0, 1'000'000));
    auto arrayDataDesc = std::make_unique<DatasetDescriptorNew>(std::move(arrayData), seed);

    TypeDistrVector arrayDataset;
    arrayDataset.push_back(
        std::make_unique<ArrDistribution>(uniform, 1.0, 100, 5, 10, std::move(arrayDataDesc)));
    DatasetDescriptorNew arrayDatasetDesc{std::move(arrayDataset), seed};

    // Build 100k values where each value is an array of length 5-10.
    const auto values = arrayDatasetDesc.genRandomDataset(100'000);
    ASSERT_EQ(100'000, values.size());

    auto ceHist = createCEHistogram(values, ScalarHistogram::kMaxBuckets);

    ASSERT_TRUE(ceHist->getScalar().empty());
    ASSERT_EQ(100, ceHist->getArrayUnique().getBuckets().size());
    ASSERT_EQ(100, ceHist->getArrayMin().getBuckets().size());
    ASSERT_EQ(100, ceHist->getArrayMax().getBuckets().size());
}

std::vector<SBEValue> generateValuesVector(std::vector<int> vals) {
    std::vector<SBEValue> ret;
    ret.reserve(vals.size());
    std::transform(vals.cbegin(), vals.cend(), std::back_inserter(ret), [](int v) {
        return makeInt64Value(v);
    });
    return ret;
}

void assertBounds(const std::vector<int>& expectedBounds, const ScalarHistogram& histogram) {
    std::vector<int> gotBounds;
    for (size_t i = 0; i < histogram.getBounds().size(); ++i) {
        [[maybe_unused]] auto [_, v] = histogram.getBounds().getAt(i);
        gotBounds.push_back(sbe::value::bitcastTo<int>(v));
    }
    ASSERT_EQ(expectedBounds, gotBounds);
}

TEST(CEHistograms, MaxDiffIntegerBounds) {
    auto values = generateValuesVector({3, 6, 9});
    auto ceHist = createCEHistogram(values, 3);
    assertBounds({3, 6, 9}, ceHist->getScalar());

    // Recall that area = (distance to next value - current value) * freqency of current value

    // Data distribution -> Area
    // 3 -> inf
    // 6 -> (7-6) * 1 = 1
    // 7 -> (9-7) * 1 = 2
    // 9 -> inf
    // We'd expect the top 3 buckets to be {3, 7, 9}.
    values = generateValuesVector({3, 6, 7, 9});
    ceHist = createCEHistogram(values, 3);
    assertBounds({3, 7, 9}, ceHist->getScalar());

    // Data distribution -> Area
    // 1 -> inf
    // 2 -> (3-2) * 1 = 1
    // 3 -> (4-3) * 10 = 10
    // 4 -> (10-4) * 1 = 6
    // 10 -> (12-10) * 1 = 2
    // 12 -> inf
    values = generateValuesVector({1, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 10, 12});
    ceHist = createCEHistogram(values, 3);
    assertBounds({1, 3, 12}, ceHist->getScalar());
}

TEST(CEHistograms, Golden) {
    const std::vector<SBEValue> values = {
        makeInt64Value(3),
        makeInt64Value(4),
        makeInt64Value(4),
        makeInt64Value(6),
        sbe::value::makeNewString("delorean"),
        sbe::value::makeNewString("delorean"),
        sbe::value::makeNewString("marty"),
        sbe::value::makeNewString("mcfly"),
        makeDateValue(dateFromISOString("1985-10-26T09:00:00+0000").getValue()),
        makeDateValue(dateFromISOString("2000-01-01T01:00:00+0000").getValue()),
        makeDateValue(dateFromISOString("2015-10-21T07:28:00+0000").getValue()),
        sbe::value::makeCopyArray([] {
            sbe::value::Array arr;
            arr.push_back(sbe::value::TypeTags::NumberInt64, 8);
            arr.push_back(sbe::value::TypeTags::NumberInt64, 8);
            arr.push_back(sbe::value::TypeTags::NumberInt64, 9);
            arr.push_back(sbe::value::TypeTags::NumberInt64, 10);
            return arr;
        }()),
    };
    auto ceHist = createCEHistogram(values, 8, stats::SortArg::kArea);
    auto expected = fromjson(R"(
    {
        trueCount: 0.0,
        falseCount: 0.0,
        nanCount: 0.0,
        emptyArrayCount: 0.0,
        typeCount: [
            { typeName: "NumberInt64", count: 4.0 },
            { typeName: "Date", count: 3.0 },
            { typeName: "StringSmall", count: 2.0 },
            { typeName: "StringBig", count: 2.0 },
            { typeName: "Array", count: 1.0 }
        ],
        scalarHistogram: {
            buckets: [
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 1.0, cumulativeDistincts: 1.0 },
                { boundaryCount: 2.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 3.0, cumulativeDistincts: 2.0 },
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 4.0, cumulativeDistincts: 3.0 },
                { boundaryCount: 2.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 6.0, cumulativeDistincts: 4.0 },
                { boundaryCount: 1.0, rangeCount: 1.0, rangeDistincts: 1.0, cumulativeCount: 8.0, cumulativeDistincts: 6.0 },
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 9.0, cumulativeDistincts: 7.0 },
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 10.0, cumulativeDistincts: 8.0 },
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 11.0, cumulativeDistincts: 9.0 }
            ],
            bounds: [ 3, 4, 6, "delorean", "mcfly", new Date(499165200000), new Date(946688400000), new Date(1445412480000) ]
        },
        arrayStatistics: {
            minHistogram: {
                buckets: [ { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 1.0, cumulativeDistincts: 1.0 } ],
                bounds: [ 8 ]
            },
            maxHistogram: {
                buckets: [ { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 1.0, cumulativeDistincts: 1.0 } ],
                bounds: [ 10 ]
            },
            uniqueHistogram: {
                buckets: [
                    { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 1.0, cumulativeDistincts: 1.0 },
                    { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 2.0, cumulativeDistincts: 2.0 },
                    { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 3.0, cumulativeDistincts: 3.0 }
                ],
                bounds: [ 8, 9, 10 ]
            },
            typeCount: [ { typeName: "NumberInt64", count: 1.0 } ]
        }
    })");
    ASSERT_BSONOBJ_EQ(expected, ceHist->serialize());

    auto ceHistAreaDiff = createCEHistogram(values, 8);
    auto expectedAreaDiff = fromjson(R"(
    {
        trueCount: 0.0,
        falseCount: 0.0,
        nanCount: 0.0,
        emptyArrayCount: 0.0,
        typeCount: [
            { typeName: "NumberInt64", count: 4.0 },
            { typeName: "Date", count: 3.0 },
            { typeName: "StringSmall", count: 2.0 },
            { typeName: "StringBig", count: 2.0 },
            { typeName: "Array", count: 1.0 }
        ],
        scalarHistogram: {
            buckets: [
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 1.0, cumulativeDistincts: 1.0 },
                { boundaryCount: 2.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 3.0, cumulativeDistincts: 2.0 },
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 4.0, cumulativeDistincts: 3.0 },
                { boundaryCount: 2.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 6.0, cumulativeDistincts: 4.0 },
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 7.0, cumulativeDistincts: 5.0 },
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 8.0, cumulativeDistincts: 6.0 },
                { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 9.0, cumulativeDistincts: 7.0 },
                { boundaryCount: 1.0, rangeCount: 1.0, rangeDistincts: 1.0, cumulativeCount: 11.0, cumulativeDistincts: 9.0 }
            ],
            bounds: [ 3, 4, 6, "delorean", "marty", "mcfly", new Date(499165200000), new Date(1445412480000) ]
        },
        arrayStatistics: {
            minHistogram: {
                buckets: [ { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 1.0, cumulativeDistincts: 1.0 } ],
                bounds: [ 8 ]
            },
            maxHistogram: {
                buckets: [ { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 1.0, cumulativeDistincts: 1.0 } ],
                bounds: [ 10 ]
            },
            uniqueHistogram: {
                buckets: [
                    { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 1.0, cumulativeDistincts: 1.0 },
                    { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 2.0, cumulativeDistincts: 2.0 },
                    { boundaryCount: 1.0, rangeCount: 0.0, rangeDistincts: 0.0, cumulativeCount: 3.0, cumulativeDistincts: 3.0 }
                ],
                bounds: [ 8, 9, 10 ]
            },
            typeCount: [ { typeName: "NumberInt64", count: 1.0 } ]
        }
    })");
    ASSERT_BSONOBJ_EQ(expectedAreaDiff, ceHistAreaDiff->serialize());
}

}  // namespace mongo::stats
