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
#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace optimizer;
using namespace cascades;

class CEHistogramTester : public CETester {
public:
    CEHistogramTester(std::string collName, double numRecords, const CollectionStatistics& stats)
        : CETester(collName, numRecords), _stats{stats} {}

protected:
    std::unique_ptr<CEInterface> getCETransport() const override {
        return std::make_unique<CEHistogramTransport>(_stats);
    }

private:
    const CollectionStatistics& _stats;
};

struct TestBucket {
    Value val;
    int equalFreq;
    int rangeFreq = 0;
    int ndv = 1; /* ndv including bucket boundary*/
};

std::unique_ptr<ArrayHistogram> getHistogramFromData(std::vector<TestBucket> testBuckets) {
    sbe::value::Array bounds;
    std::vector<Bucket> buckets;
    std::map<sbe::value::TypeTags, size_t> typeCounts;

    int cumulativeFreq = 0;
    int cumulativeNDV = 0;
    for (auto b : testBuckets) {
        // Add bucket boundary value to bounds.
        auto sbeVal = stage_builder::makeValue(b.val);
        auto [tag, val] = sbeVal;
        bounds.push_back(tag, val);

        // Increment count of values for each type tag.
        if (auto it = typeCounts.find(tag); it != typeCounts.end()) {
            it->second += b.equalFreq + b.rangeFreq;
        } else {
            typeCounts[tag] = b.equalFreq + b.rangeFreq;
        }
        cumulativeFreq += b.equalFreq + b.rangeFreq;
        cumulativeNDV += b.ndv;

        // Create a histogram bucket.
        buckets.emplace_back(b.equalFreq,
                             b.rangeFreq,
                             cumulativeFreq,
                             b.ndv - 1, /* ndv excluding bucket boundary*/
                             cumulativeNDV);
    }

    return std::make_unique<ArrayHistogram>(ScalarHistogram(std::move(bounds), std::move(buckets)),
                                            std::move(typeCounts));
}

TEST(CEHistogramTest, AssertSmallMaxDiffHistogramEstimatesAtomicPredicates) {
    const auto collName = "test";
    const auto collCardinality = 8;

    CollectionStatistics collStats(collCardinality);

    // Construct a histogram with two buckets: one for 3 ints equal to 1, another for 5 strings
    // equal to "ing".
    const std::string& str = "ing";
    collStats.addHistogram("a",
                           getHistogramFromData({
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
    const auto collName = "test";
    const auto collCardinality = 9;

    CollectionStatistics collStats(collCardinality);

    // Construct a histogram with three int buckets for field 'a'.
    collStats.addHistogram("a",
                           getHistogramFromData({
                               {Value(1), 3 /* frequency */},
                               {Value(2), 5 /* frequency */},
                               {Value(3), 1 /* frequency */},
                           }));

    // Construct a histogram with two int buckets for field 'b'.
    collStats.addHistogram("b",
                           getHistogramFromData({
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
    const auto collName = "test";
    const auto collCardinality = 0;

    CollectionStatistics collStats(collCardinality);
    collStats.addHistogram("empty", std::make_unique<ArrayHistogram>());
    CEHistogramTester t(collName, collCardinality, collStats);

    ASSERT_MATCH_CE(t, "{empty: {$eq: 1.0}}", 0.0);
    ASSERT_MATCH_CE(t, "{empty: {$lt: 1.0}, empty: {$gt: 0.0}}", 0.0);
    ASSERT_MATCH_CE(t, "{empty: {$eq: 1.0}, other: {$eq: \"anything\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{other: {$eq: \"anything\"}, empty: {$eq: 1.0}}", 0.0);
}

TEST(CEHistogramTest, AssertOneBucketOneIntHistogram) {
    const auto collName = "test";
    const auto collCardinality = 50;

    CollectionStatistics collStats(collCardinality);

    // Create a histogram with a single bucket that contains exactly one int (42) with a frequency
    // of 50 (equal to the collection cardinality).
    collStats.addHistogram("soloInt",
                           getHistogramFromData({
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
    // TODO: investigate why this returns 5?
    // ASSERT_MATCH_CE(t, "{soloInt: {$eq: [42]}}", 0.0);
    // TODO: investigate why this line triggers an out of memory exception.
    // ASSERT_MATCH_CE(t, "{soloInt: {$eq: {soloInt: 42}}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: \"42\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: \"42\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: 42.1}}", collCardinality);
}

TEST(CEHistogramTest, AssertOneBoundIntRangeHistogram) {
    const auto collName = "test";
    const auto collCardinality = 51;

    CollectionStatistics collStats(collCardinality);

    collStats.addHistogram(
        "intRange",
        getHistogramFromData({
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
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 15}}", 23.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 15}}", 23.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 11}, intRange: {$lte: 20}}", 23.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 11}, intRange: {$lte: 20}}", 23.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 11}, intRange: {$lt: 20}}", 23.27);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 11}, intRange: {$lt: 20}}", 23.27);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lt: 15}}", 17.25);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lt: 15}}", 17.25);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lte: 15}}", 17.25);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lte: 15}}", 17.25);

    // Test ranges that partially overlap with the entire histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 11}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 8}, intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 8}, intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 8}, intRange: {$lt: 15}}", 27.5);
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
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 19}, intRange: {$gt: 11}}", 17.26);

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
}

}  // namespace
}  // namespace mongo::ce
