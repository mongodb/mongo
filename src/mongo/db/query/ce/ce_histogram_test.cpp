/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/ce/ce_histogram.h"
#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/ce/collection_statistics_mock.h"
#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace optimizer;
using namespace cascades;

std::string collName("test");

class CEHistogramTester : public CETester {
public:
    CEHistogramTester(std::string collName,
                      double numRecords,
                      std::shared_ptr<CollectionStatistics> stats)
        : CETester(collName, numRecords), _stats{stats} {}

protected:
    std::unique_ptr<CEInterface> getCETransport() const override {
        // making a copy of CollecitonStatistics to override
        return std::make_unique<CEHistogramTransport>(_stats);
    }

private:
    std::shared_ptr<CollectionStatistics> _stats;
};

struct TestBucket {
    Value val;
    int equalFreq;
    int rangeFreq = 0;
    int ndv = 1; /* ndv including bucket boundary*/
};

ScalarHistogram getHistogramFromData(std::vector<TestBucket> testBuckets) {
    sbe::value::Array bounds;
    std::vector<Bucket> buckets;

    int cumulativeFreq = 0;
    int cumulativeNDV = 0;
    for (const auto& b : testBuckets) {
        // Add bucket boundary value to bounds.
        auto [tag, val] = stage_builder::makeValue(b.val);
        bounds.push_back(tag, val);

        cumulativeFreq += b.equalFreq + b.rangeFreq;
        cumulativeNDV += b.ndv;

        // Create a histogram bucket.
        buckets.emplace_back(b.equalFreq,
                             b.rangeFreq,
                             cumulativeFreq,
                             b.ndv - 1, /* ndv excluding bucket boundary*/
                             cumulativeNDV);
    }

    return ScalarHistogram(std::move(bounds), std::move(buckets));
}

TypeCounts getTypeCountsFromData(std::vector<TestBucket> testBuckets) {
    TypeCounts typeCounts;
    for (const auto& b : testBuckets) {
        // Add bucket boundary value to bounds.
        auto sbeVal = stage_builder::makeValue(b.val);
        auto [tag, val] = sbeVal;

        // Increment count of values for each type tag.
        if (auto it = typeCounts.find(tag); it != typeCounts.end()) {
            it->second += b.equalFreq + b.rangeFreq;
        } else {
            typeCounts[tag] = b.equalFreq + b.rangeFreq;
        }
    }
    return typeCounts;
}

std::unique_ptr<ArrayHistogram> getArrayHistogramFromData(std::vector<TestBucket> testBuckets) {
    return std::make_unique<ArrayHistogram>(getHistogramFromData(testBuckets),
                                            getTypeCountsFromData(testBuckets));
}

std::unique_ptr<ArrayHistogram> getArrayHistogramFromData(
    std::vector<TestBucket> scalarBuckets,
    std::vector<TestBucket> arrayUniqueBuckets,
    std::vector<TestBucket> arrayMinBuckets,
    std::vector<TestBucket> arrayMaxBuckets,
    TypeCounts arrayTypeCounts,
    size_t emptyArrayCount = 0) {
    auto arrayMinHist = getHistogramFromData(arrayMinBuckets);
    auto arrayMaxHist = getHistogramFromData(arrayMaxBuckets);
    TypeCounts typeCounts = getTypeCountsFromData(scalarBuckets);
    // The min/max histograms contain one (min or max) value from each array. Therefore the
    // total number of min/max values is equal to the number of non non-empty arrays.
    // The total number of arrays needs to account for the additional empty arrays not present
    // in histograms.
    typeCounts[value::TypeTags::Array] = arrayMinHist.getCardinality() + emptyArrayCount;
    return std::make_unique<ArrayHistogram>(getHistogramFromData(scalarBuckets),
                                            std::move(typeCounts),
                                            getHistogramFromData(arrayUniqueBuckets),
                                            std::move(arrayMinHist),
                                            std::move(arrayMaxHist),
                                            std::move(arrayTypeCounts));
}

TEST(CEHistogramTest, AssertSmallMaxDiffHistogramEstimatesAtomicPredicates) {
    const auto collCardinality = 8;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));

    // Construct a histogram with two buckets: one for 3 ints equal to 1, another for 5 strings
    // equal to "ing".
    const std::string& str = "ing";
    collStats->addHistogram("a",
                            getArrayHistogramFromData({
                                {Value(1), 3 /* frequency */},
                                {Value(str), 5 /* frequency */},
                            }));

    CEHistogramTester t(collName, collCardinality, collStats);

    // Test $eq.
    ASSERT_MATCH_CE(t, "{a: {$eq: 1}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$eq: 2}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$eq: \"ing\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$eq: \"foo\"}}", 0.0);

    // Test case when field doesn't match fieldpath of histogram. This falls back to heuristics.
    ASSERT_MATCH_CE(t, "{b: {$eq: 1}}", 2.82843);

    // Test $gt.
    ASSERT_MATCH_CE(t, "{a: {$gt: 3}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 1}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 0}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: \"bar\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: \"ing\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: \"zap\"}}", 0.0);

    // Test $lt.
    ASSERT_MATCH_CE(t, "{a: {$lt: 3}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: 1}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: 0}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: \"bar\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: \"ing\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: \"zap\"}}", 5.0);

    // Test $gte.
    ASSERT_MATCH_CE(t, "{a: {$gte: 3}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: 1}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: 0}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: \"bar\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: \"ing\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: \"zap\"}}", 0.0);

    // Test $lte.
    ASSERT_MATCH_CE(t, "{a: {$lte: 3}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: 1}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: 0}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: \"bar\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: \"ing\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: \"zap\"}}", 5.0);
}

TEST(CEHistogramTest, AssertSmallHistogramEstimatesComplexPredicates) {
    const auto collCardinality = 9;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));

    // Construct a histogram with three int buckets for field 'a'.
    collStats->addHistogram("a",
                            getArrayHistogramFromData({
                                {Value(1), 3 /* frequency */},
                                {Value(2), 5 /* frequency */},
                                {Value(3), 1 /* frequency */},
                            }));

    // Construct a histogram with two int buckets for field 'b'.
    collStats->addHistogram("b",
                            getArrayHistogramFromData({
                                {Value(22), 3 /* frequency */},
                                {Value(33), 6 /* frequency */},
                            }));

    CEHistogramTester t(collName, collCardinality, collStats);

    // Test simple conjunctions on one field. Note the first example: the range we expect to see
    // here is (1, 3); however, the structure in the SargableNode gives us a conjunction of two
    // intervals instead: (1, "") ^ (nan, 3) This is then estimated using exponential backoff to
    // give us a less accurate result. The correct cardinality here would be 5.
    ASSERT_MATCH_CE(t, "{a: {$gt: 1}, a: {$lt: 3}}", 5.66);
    ASSERT_MATCH_CE(t, "{a: {$gt: 1}, a: {$lte: 3}}", 6.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: 1}, a: {$lt: 3}}", 8.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: 1}, a: {$lte: 3}}", 9.0);

    // Test ranges which exclude each other.
    ASSERT_MATCH_CE(t, "{a: {$lt: 1}, a: {$gt: 3}}", 0.0);

    // Test overlapping ranges. This is a similar case to {a: {$gt: 1}, a: {$lt: 3}} above: we
    // expect to see the range [2, 2]; instead, we see the range [nan, 2] ^ [2, "").
    ASSERT_MATCH_CE(t, "{a: {$lte: 2}, a: {$gte: 2}}", 5.66);

    // Test conjunctions over multiple fields for which we have histograms. Here we expect a
    // cardinality estimated by exponential backoff.
    ASSERT_MATCH_CE(t, "{a: {$eq: 2}, b: {$eq: 22}}", 2.24);
    ASSERT_MATCH_CE(t, "{a: {$eq: 11}, b: {$eq: 22}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 11}, a: {$lte: 100}, b: {$eq: 22}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: 3}, a: {$gte: 1}, b: {$lt: 100}, b: {$gt: 30}}", 5.66);

    // Test conjunctions over multiple fields for which we may not have histograms. This falls back
    // to heuristic estimation.
    ASSERT_MATCH_CE(t, "{a: {$eq: 2}, c: {$eq: 1}}", 1.73205);
    ASSERT_MATCH_CE(t, "{c: {$eq: 2}, d: {$eq: 22}}", 1.73205);
}

TEST(CEHistogramTest, SanityTestEmptyHistogram) {
    const auto collCardinality = 0;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));
    collStats->addHistogram("empty", std::make_unique<ArrayHistogram>());
    CEHistogramTester t(collName, collCardinality, collStats);

    ASSERT_MATCH_CE(t, "{empty: {$eq: 1.0}}", 0.0);
    ASSERT_MATCH_CE(t, "{empty: {$lt: 1.0}, empty: {$gt: 0.0}}", 0.0);
    ASSERT_MATCH_CE(t, "{empty: {$eq: 1.0}, other: {$eq: \"anything\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{other: {$eq: \"anything\"}, empty: {$eq: 1.0}}", 0.0);
}

TEST(CEHistogramTest, TestOneBucketOneIntHistogram) {
    const auto collName = "test";
    const auto collCardinality = 50;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));

    // Create a histogram with a single bucket that contains exactly one int (42) with a frequency
    // of 50 (equal to the collection cardinality).
    collStats->addHistogram("soloInt",
                            getArrayHistogramFromData({
                                {Value(42), collCardinality /* frequency */},
                            }));

    CEHistogramTester t(collName, collCardinality, collStats);

    // Check against a variety of intervals that include 42 as a bound.
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: 42}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lte: 42}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}, soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}, soloInt: {$lte: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}, soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}, soloInt: {$lte: 42}}", collCardinality);

    // Check against a variety of intervals that include 42 only as one bound.
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}, soloInt: {$lt: 43}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}, soloInt: {$lte: 43}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}, soloInt: {$lt: 43}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}, soloInt: {$lte: 43}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}, soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}, soloInt: {$lte: 42}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}, soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}, soloInt: {$lte: 42}}", collCardinality);

    // Check against a variety of intervals close to 42 using a lower bound of 41 and a higher bound
    // of 43.
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: 41}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: 43}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: 43}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$lte: 43}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}, soloInt: {$lt: 43}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}, soloInt: {$lt: 43}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}, soloInt: {$lte: 43}}", collCardinality);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}, soloInt: {$lte: 43}}", collCardinality);

    // Check against different types.
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: \"42\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: \"42\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: 42.1}}", collCardinality);
}

TEST(CEHistogramTest, TestOneBoundIntRangeHistogram) {
    const auto collName = "test";
    const auto collCardinality = 51;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));

    collStats->addHistogram(
        "intRange",
        getArrayHistogramFromData({
            {Value(10), 5 /* frequency */},
            {Value(20), 1 /* frequency */, 45 /* range frequency */, 10 /* ndv */},
        }));

    CEHistogramTester t(collName, collCardinality, collStats);

    // Test ranges that overlap only with the lower bound.
    // Note: 5 values equal 10.
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 10}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 10}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 10}, intRange: {$gte: 10}}", 5.0);

    // Test ranges that overlap only with the upper bound.
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 11}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 15}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 15.5}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 20}}", 1.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 20}}", 1.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 10}}", 46.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 15}}", 28.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 15}}", 23.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 11}, intRange: {$lte: 20}}", 41.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 11}, intRange: {$lte: 20}}", 41.5);

    // Test ranges that partially overlap with the entire histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 11}}", 9.5);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 15}}", 22.5);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 8}, intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 8}, intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 8}, intRange: {$lt: 15}}", 22.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 8}, intRange: {$lte: 15}}", 27.5);

    // Test ranges that include all values in the histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 10}, intRange: {$lte: 20}}", collCardinality);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 1}, intRange: {$lte: 30}}", collCardinality);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 1}, intRange: {$lt: 30}}", collCardinality);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 1}, intRange: {$lte: 30}}", collCardinality);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 1}, intRange: {$lt: 30}}", collCardinality);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 0}}", collCardinality);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 0}}", collCardinality);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 100}}", collCardinality);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 100}}", collCardinality);

    // Test ranges that are fully included in the histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 10.5}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 12.5}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 19.36}}", 5.0);

    // Test ranges that don't overlap with the histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 10}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 20.1}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 21}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 21}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 20}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 100}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 30}, intRange: {$lte: 50}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 30}, intRange: {$lt: 50}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 30}, intRange: {$lt: 50}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 30}, intRange: {$lte: 50}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 0}, intRange: {$lte: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 0}, intRange: {$lt: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 0}, intRange: {$lt: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 0}, intRange: {$lte: 5}}", 0.0);

    // Because we don't specify any indexes here, these intervals do not go through simplification.
    // This means that instead of having one key in the requirements map of the generated sargable
    // node corresponding to the path "intRange", we have two keys and two ranges, both
    // corresponding to the same path. As a consequence, we combine the estimates for the intervals
    // using exponential backoff, which results in an overestimate.
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 11}, intRange: {$lt: 20}}", 41.09);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 11}, intRange: {$lt: 20}}", 41.09);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lt: 15}}", 19.16);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lt: 15}}", 20.42);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lte: 15}}", 23.42);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lte: 15}}", 24.96);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 19}, intRange: {$gt: 11}}", 36.53);

    // When we specify that there is a non-multikey index on 'intRange', we expect to see interval
    // simplification occurring, which should provide a better estimate for the following ranges.
    t.setIndexes(
        {{"intRangeIndex",
          makeIndexDefinition("intRange", CollationOp::Ascending, /* isMultiKey */ false)}});
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 11}, intRange: {$lt: 20}}", 40.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 11}, intRange: {$lt: 20}}", 40.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lt: 15}}", 8.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lt: 15}}", 13.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lte: 15}}", 13.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lte: 15}}", 18.5);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 19}, intRange: {$gt: 11}}", 31.0);
}

TEST(CEHistogramTest, TestHistogramOnNestedPaths) {
    const auto collCardinality = 50;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));

    // Create a histogram with a single bucket that contains exactly one int (42) with a frequency
    // of 50 (equal to the collection cardinality).
    collStats->addHistogram("path",
                            getArrayHistogramFromData({
                                {Value(42), collCardinality /* frequency */},
                            }));
    collStats->addHistogram("a.histogram.path",
                            getArrayHistogramFromData({
                                {Value(42), collCardinality /* frequency */},
                            }));

    CEHistogramTester t(collName, collCardinality, collStats);

    ASSERT_MATCH_CE(t, "{\"not.a.histogram.path\": {$eq: 42}}", 7.071 /* heuristic */);
    ASSERT_MATCH_CE(t, "{\"a.histogram.path\": {$eq: 42}}", collCardinality);
    ASSERT_MATCH_CE(
        t, "{\"a.histogram.path.with.no.histogram\": {$eq: 42}}", 7.071 /* heuristic */);

    // TODO SERVER-68596: this doesn't generate a SargableNode. When it does, it should return 0.0.
    // Currently, this estimate falls back to heuristic CE.
    ASSERT_MATCH_CE(t, "{\"a.histogram.path\": {$elemMatch: {$eq: 42}}}", 6.21);
}

TEST(CEHistogramTest, TestArrayHistogramOnAtomicPredicates) {
    const auto collName = "test";
    const auto collCardinality = 6;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));

    collStats->addHistogram(
        "a",
        // Generate a histogram for this data:
        // {a: 1}, {a: 2}, {a: [1, 2, 3, 2, 2]}, {a: [10]}, {a: [2, 3, 3, 4, 5, 5, 6]}, {a: []}
        //  - scalars: [1, 2]
        //  - unique values: [1], [2], [1, 2, 3], [10], [2, 3, 4, 5, 6]
        //      -> [1, 1, 2, 2, 2, 3, 3, 4, 5, 6, 10]
        //  - min values: [1], [10], [2] -> [1, 1, 2, 2, 10]
        //  - max values: [3], [10], [6] -> [1, 2, 3, 6, 10]
        getArrayHistogramFromData(
            {// Scalar buckets.
             {Value(1), 1 /* frequency */},
             {Value(2), 1 /* frequency */}},
            {
                // Array unique buckets.
                {Value(1), 1 /* frequency */},
                {Value(2), 2 /* frequency */},
                {Value(3), 2 /* frequency */},
                {Value(4), 1 /* frequency */},
                {Value(5), 1 /* frequency */},
                {Value(6), 1 /* frequency */},
                {Value(10), 1 /* frequency */},
            },
            {
                // Array min buckets.
                {Value(1), 1 /* frequency */},
                {Value(2), 1 /* frequency */},
                {Value(10), 1 /* frequency */},
            },
            {
                // Array max buckets.
                {Value(3), 1 /* frequency */},
                {Value(6), 1 /* frequency */},
                {Value(10), 1 /* frequency */},
            },
            {{sbe::value::TypeTags::NumberInt32, 4}},  // Array type counts.
            1                                          // 1 empty array.
            ));

    CEHistogramTester t(collName, collCardinality, collStats);

    // Test simple predicates against 'a'. Note: in the $elemMatch case, we exclude scalar
    // estimates. Without $elemMatch, we add the array histogram and scalar histogram estimates
    // together.

    // Test equality predicates.
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0 /* CE */, 0.0 /* $elemMatch CE */, "a", "{$eq: 0}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, 3.0 /* CE */, 2.0 /* $elemMatch CE */, "a", "{$eq: 2}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 2.0 /* $elemMatch CE */, "a", "{$eq: 3}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 4}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 6}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 10}");
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0 /* CE */, 0.0 /* $elemMatch CE */, "a", "{$eq: 11}");

    // Test histogram boundary values.
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0 /* CE */, 0.0 /* $elemMatch CE */, "a", "{$lt: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$lte: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0 /* CE */, 0.0 /* $elemMatch CE */, "a", "{$gt: 10}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$gte: 10}");

    ASSERT_EQ_ELEMMATCH_CE(t, 5.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$lte: 10}");
    ASSERT_EQ_ELEMMATCH_CE(t, 4.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$lt: 10}");
    ASSERT_EQ_ELEMMATCH_CE(t, 4.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$gt: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, 5.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$gte: 1}");

    ASSERT_EQ_ELEMMATCH_CE(t, 4.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$lte: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 4.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$lt: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 2.0 /* $elemMatch CE */, "a", "{$gt: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 2.55 /* $elemMatch CE */, "a", "{$gte: 5}");

    ASSERT_EQ_ELEMMATCH_CE(t, 2.45 /* CE */, 2.55 /* $elemMatch CE */, "a", "{$gt: 2, $lt: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 3.27 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$gte: 2, $lt: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.45 /* CE */, 3.40 /* $elemMatch CE */, "a", "{$gt: 2, $lte: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 3.27 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$gte: 2, $lte: 5}");
}

TEST(CEHistogramTest, TestArrayHistogramOnCompositePredicates) {
    const auto collName = "test";
    const auto collCardinality = 175;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));

    // A scalar histogram with values in the range [1,10], most of which are in the middle bucket.
    collStats->addHistogram(
        "scalar",
        getArrayHistogramFromData({
            {Value(1), 10 /* frequency */},
            {Value(2), 10 /* frequency */},
            {Value(3), 20 /* frequency */, 120 /* range frequency */, 5 /* ndv */},
            {Value(8), 5 /* frequency */, 10 /* range frequency */, 3 /* ndv */},
        }));

    // An array histogram built on the following arrays with 35 occurrences of each:
    // [{[1, 2, 3]: 35}, {[5, 5, 5, 5, 5]: 35}, {[6]: 35}, {[]: 35}, {[8, 9, 10]: 35}]
    collStats->addHistogram(
        "array",
        getArrayHistogramFromData(
            {/* No scalar buckets. */},
            {
                // Array unique buckets.
                {Value(2), 35 /* frequency */, 35 /* range frequency */, 2 /* ndv */},
                {Value(5), 35 /* frequency */, 35 /* range frequency */, 2 /* ndv */},
                {Value(6), 35 /* frequency */},
                {Value(10), 35 /* frequency */, 105 /* range frequency */, 3 /* ndv */},
            },
            {
                // Array min buckets.
                {Value(1), 35 /* frequency */},
                {Value(5), 35 /* frequency */},
                {Value(6), 35 /* frequency */},
                {Value(8), 35 /* frequency */},
            },
            {
                // Array max buckets.
                {Value(3), 35 /* frequency */},
                {Value(5), 35 /* frequency */},
                {Value(6), 35 /* frequency */},
                {Value(10), 35 /* frequency */},
            },
            {{sbe::value::TypeTags::NumberInt32, collCardinality}},  // Array type counts.
            35                                                       // 35 empty arrays
            ));

    collStats->addHistogram(
        "mixed",
        // The mixed histogram has 87 scalars that follow approximately the same distribution as
        // in the pure scalar case, and 88 arrays with the following distribution:
        //  [{[1, 2, 3]: 17}, {[5, 5, 5, 5, 5]: 17}, {[6]: 17}, {[]: 20}, {[8, 9, 10]: 17}]
        getArrayHistogramFromData(
            {
                // Scalar buckets. These are half the number of values from the "scalar" histogram.
                {Value(1), 5 /* frequency */},
                {Value(2), 5 /* frequency */},
                {Value(3), 10 /* frequency */, 60 /* range frequency */, 5 /* ndv */},
                {Value(8), 2 /* frequency */, 5 /* range frequency */, 3 /* ndv */},
            },
            {
                // Array unique buckets.
                {Value(2), 17 /* frequency */, 17 /* range frequency */, 2 /* ndv */},
                {Value(5), 17 /* frequency */, 17 /* range frequency */, 2 /* ndv */},
                {Value(6), 17 /* frequency */},
                {Value(10), 17 /* frequency */, 34 /* range frequency */, 3 /* ndv */},
            },
            {
                // Array min buckets.
                {Value(1), 17 /* frequency */},
                {Value(5), 17 /* frequency */},
                {Value(6), 17 /* frequency */},
                {Value(8), 17 /* frequency */},
            },
            {
                // Array max buckets.
                {Value(3), 17 /* frequency */},
                {Value(5), 17 /* frequency */},
                {Value(6), 17 /* frequency */},
                {Value(10), 17 /* frequency */},
            },
            {{sbe::value::TypeTags::NumberInt32, 88}},  // Array type counts.
            20                                          // 20 empty arrays.
            ));

    CEHistogramTester t(collName, collCardinality, collStats);

    // Test cardinality of individual predicates.
    ASSERT_EQ_ELEMMATCH_CE(t, 5.0 /* CE */, 0.0 /* $elemMatch CE */, "scalar", "{$eq: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 35.0 /* CE */, 35.0 /* $elemMatch CE */, "array", "{$eq: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 19.5 /* CE */, 17.0 /* $elemMatch CE */, "mixed", "{$eq: 5}");

    // Test cardinality of predicate combinations; the following tests make sure we correctly track
    // which paths have $elemMatches and which don't. Some notes:
    //  - Whenever we use 'scalar' + $elemMatch, we expect an estimate of 0 because $elemMatch never
    // returns documents on non-array paths.
    //  - Whenever we use 'mixed' + $elemMatch, we expect the estimate to decrease because we omit
    // scalar values in 'mixed' from our estimate.
    //  - We do not expect the estimate on 'array' to be affected by the presence of $elemMatch,
    // since we only have array values for this field.

    // Composite predicate on 'scalar' and 'array' fields.
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, array: {$eq: 5}}", 2.236);
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, array: {$elemMatch: {$eq: 5}}}", 2.236);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 0.0);

    // Composite predicate on 'mixed' and 'array' fields.
    ASSERT_MATCH_CE(t, "{mixed: {$eq: 5}, array: {$eq: 5}}", 8.721);
    ASSERT_MATCH_CE(t, "{mixed: {$eq: 5}, array: {$elemMatch: {$eq: 5}}}", 8.721);
    ASSERT_MATCH_CE(t, "{mixed: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 7.603);

    // Composite predicate on 'scalar' and 'mixed' fields.
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$eq: 5}}", 1.669);
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$elemMatch: {$eq: 5}}}", 1.559);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$eq: 5}}", 0.0);

    // Composite predicate on all three fields without '$elemMatch' on 'array'.
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$eq: 5}, array: {$eq: 5}}", 1.116);
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 1.042);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$eq: 5}, array: {$eq: 5}}", 0.0);

    // Composite predicate on all three fields with '$elemMatch' on 'array' (same expected results
    // as above).
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$eq: 5}, array: {$elemMatch: {$eq: 5}}}", 1.116);

    // Test case where the same path has both $match and $elemMatch (same as $elemMatch case).
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, scalar: {$eq: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{mixed: {$elemMatch: {$eq: 5}}, mixed: {$eq: 5}}", 17.0);
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 35.0);

    // Test case with multiple predicates and ranges.
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$lt: 5}}, mixed: {$lt: 5}}", 70.2156);
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$lt: 5}}, mixed: {$gt: 5}}", 28.4848);

    // Test multiple $elemMatches.
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, array: {$elemMatch: {$eq: 5}}}", 0.0);
    ASSERT_MATCH_CE(t, "{mixed: {$elemMatch: {$eq: 5}}, array: {$elemMatch: {$eq: 5}}}", 7.603);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$elemMatch: {$eq: 5}}}", 0.0);
    ASSERT_MATCH_CE(
        t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 0.0);
    ASSERT_MATCH_CE(
        t,
        "{scalar: {$eq: 5}, mixed: {$elemMatch: {$eq: 5}}, array: {$elemMatch: {$eq: 5}}}",
        1.042);
    ASSERT_MATCH_CE(
        t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$eq: 5}, array: {$elemMatch: {$eq: 5}}}", 0.0);
    ASSERT_MATCH_CE(t,
                    "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$elemMatch: {$eq: 5}}, array: "
                    "{$elemMatch: {$eq: 5}}}",
                    0.0);
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$lt: 5}}, mixed: {$elemMatch: {$lt: 5}}}", 34.1434);
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$lt: 5}}, mixed: {$elemMatch: {$gt: 5}}}", 45.5246);

    // Verify that we still return an estimate of 0.0 for any $elemMatch predicate on a scalar
    // field when we have a non-multikey index.
    t.setIndexes({{"aScalarIndex",
                   makeIndexDefinition("scalar", CollationOp::Ascending, /* isMultiKey */ false)}});
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}}", 0.0);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$gt: 1, $lt: 10}}}", 0.0);

    // Test how we estimate singular PathArr sargable predicate.
    ASSERT_MATCH_CE_NODE(t, "{array: {$elemMatch: {}}}", 175.0, optimizer::SargableNode);
    ASSERT_MATCH_CE_NODE(t, "{mixed: {$elemMatch: {}}}", 88.0, optimizer::SargableNode);

    // Take into account both empty and non-empty arrays.
    auto makePathArrABT = [&](const std::string& path) {
        const auto scanProjection = "scan_0";
        auto scanNode = make<ScanNode>(scanProjection, collName);
        auto filterNode = make<FilterNode>(
            make<EvalFilter>(make<PathGet>(path, make<PathArr>()), make<Variable>(scanProjection)),
            std::move(scanNode));
        return make<RootNode>(
            properties::ProjectionRequirement{ProjectionNameVector{scanProjection}},
            std::move(filterNode));
    };

    // There are no arrays in the 'scalar' field.
    ABT scalarABT = makePathArrABT("scalar");
    ASSERT_CE(t, scalarABT, 0.0);

    // About half the values of this field are arrays.
    ABT mixedABT = makePathArrABT("mixed");
    ASSERT_CE(t, mixedABT, 88.0);

    // This field is always an array.
    ABT arrayABT = makePathArrABT("array");
    ASSERT_CE(t, arrayABT, collCardinality);
}

TEST(CEHistogramTest, TestMixedElemMatchAndNonElemMatch) {
    const auto collName = "test";
    const auto collCardinality = 1;

    std::shared_ptr<CollectionStatistics> collStats(new CollectionStatisticsMock(collCardinality));

    // A very simple histogram encoding a collection with one document {a: [3, 10]}.
    collStats->addHistogram("a",
                            getArrayHistogramFromData({/* No scalar buckets. */},
                                                      {
                                                          // Array unique buckets.
                                                          {Value(3), 1 /* frequency */},
                                                          {Value(10), 1 /* frequency */},
                                                      },
                                                      {
                                                          // Array min buckets.
                                                          {Value(3), 1 /* frequency */},
                                                      },
                                                      {
                                                          // Array max buckets.
                                                          {Value(10), 1 /* frequency */},
                                                      },
                                                      {{sbe::value::TypeTags::NumberInt32,
                                                        collCardinality}}  // Array type counts.
                                                      ));

    CEHistogramTester t(collName, collCardinality, collStats);

    // Tests without indexes.
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$gt: 3, $lt: 10}}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$eq: 3}, $gt: 3, $lt: 10}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10, $elemMatch: {$eq: 3}}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10, $elemMatch: {$gt: 3, $lt: 10}}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$gt: 3, $lt: 10}, $gt: 3, $lt: 10}}", 0.0);

    // Tests with multikey index (note that the index on "a" must be multikey due to arrays).
    t.setIndexes(
        {{"anIndex", makeIndexDefinition("a", CollationOp::Ascending, /* isMultiKey */ true)}});
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$gt: 3, $lt: 10}}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$eq: 3}, $gt: 3, $lt: 10}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10, $elemMatch: {$eq: 3}}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10, $elemMatch: {$gt: 3, $lt: 10}}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$gt: 3, $lt: 10}, $gt: 3, $lt: 10}}", 0.0);
}

}  // namespace
}  // namespace mongo::ce
