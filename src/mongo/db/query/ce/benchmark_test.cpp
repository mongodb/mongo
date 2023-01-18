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

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/db/query/ce/benchmark_utils.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stats/max_diff.h"
#include "mongo/db/query/stats/rand_utils.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo::optimizer::ce {
namespace {
/**
 * A name of test collection. The collection should not physically exists as its only used at the
 * metadata level.
 */
const std::string kCollName("test");

/**
 * A couple constants representing small and big string values to be put into a bucket.
 */
const size_t kSmallStrLen = 100;
const size_t kBigStrLen = 1000;
const std::string kSmallStr = stats::genString(kSmallStrLen, 0 /*seed*/);
const std::string kBigStr = stats::genString(kBigStrLen, 0 /*seed**/);

/**
 * Different number of buckets per histogram.
 */
const size_t kSmallBucketCounts = 10;
const size_t kMediumBucketCounts = 100;
const size_t kLargeBucketCounts = 1000;

/**
 * We assume our test collection contains that number of documents.
 */
const CEType kCollCardinality{10000};

/**
 * How many times to run each benchmark.
 */
const size_t kNumIterations = 1;

/**
 * When we generate data to build histograms from, the integer values are picked from a range
 * from [1, kCollCardinality / 4]. The following constants define the min, max and middle values
 * in this range.
 */
const int kMinIntValue = 1;
const int kMaxIntValue = kCollCardinality._value / 4;
const int kMiddleIntValue = kMaxIntValue / 2;

/**
 * A helper utility to generate a Scalar histogram holding 'bucketCount' buckets with
 * values of the given 'valueType'.
 */
std::shared_ptr<const stats::ArrayHistogram> generateHistogram(int bucketCount,
                                                               BucketValueType valueType,
                                                               bool includeArrays = false) {
    const auto intRatio = [&]() -> double {
        switch (valueType) {
            case BucketValueType::Int:
                return 1.0;
            case BucketValueType::MixedIntStr:
                return 0.5;
            default:
                return 0.0;
        }
    }();
    const auto [strRatio, strLen] = [&]() -> std::pair<double, boost::optional<size_t>> {
        switch (valueType) {
            case BucketValueType::SmallStr:
                return {1.0, kSmallStrLen};
            case BucketValueType::BigStr:
                return {1.0, kBigStrLen};
            case BucketValueType::MixedIntStr:
                return {0.5, kSmallStrLen};
            default:
                return {0.0, {}};
        }
    }();

    auto rawData = stats::genFixedValueArray(kCollCardinality._value, intRatio, strRatio, strLen);
    return stats::createArrayEstimator(
        includeArrays ? nestArrays(rawData, 0 /* No empty arrays */) : rawData, bucketCount);
}

/**
 * Generates a number of histograms based on the information provided in the benchmark 'descriptor'
 * and places them into the collection stats object 'collStats'.
 */
void generateHistorgrams(const BenchmarkDescriptor& descriptor,
                         std::shared_ptr<stats::CollectionStatistics> collStats) {
    for (auto&& [fieldName, valueType] : descriptor.valueTypes) {
        const bool includeArrays = [& fieldName = fieldName, &descriptor]() {
            for (auto&& [indexName, indexDef] : descriptor.indexes) {
                for (auto&& entry : indexDef.getCollationSpec())
                    if (entry._path ==
                        makeIndexPath(FieldPathType{FieldNameType{fieldName}},
                                      indexDef.isMultiKey())) {
                        return true;
                    }
            }
            return false;
        }();
        collStats->addHistogram(fieldName,
                                generateHistogram(descriptor.numBuckets, valueType, includeArrays));
    }
}

/**
 * A test fixture for CE benchmaarks. It provides a common 'setUp' hook to be invoked before each
 * benchmark, as well as a 'runBenchmarks' driver to be used in each TEST_F.
 */
class CEBenchmarkTest : public LockerNoopServiceContextTest {
protected:
    /**
     * Given a 'benchmarkName' name (which corresponds to a test name specified in a TEST_F
     * instance), a number of iterations to run, and a 'query' string holding a filter specification
     * for a $match pipeline stage, execute all registered benchmark scenarios and store the results
     * within a 'BenchmarkResultsAggregator'. The later can be used to print the results of all
     * executed benchmarks at the end of this suite as the very last test.
     */
    void runBenchmarks(BenchmarkDescriptor descriptor, const std::string& query) {
        std::shared_ptr<stats::CollectionStatistics> collStats =
            std::make_shared<stats::CollectionStatisticsMock>(kCollCardinality._value);

        std::map<std::string, std::vector<std::unique_ptr<Benchmark>>> allBenchmarks;
        allBenchmarks["histograms"] = makeVector<std::unique_ptr<Benchmark>>(
            std::make_unique<FullOptimizerBenchmark>(
                kCollName, kCollCardinality, makeHistogramEstimatorFactoryFn(collStats)),
            std::make_unique<DeriveCEBenchmark>(
                kCollName, kCollCardinality, makeHistogramEstimatorFactoryFn(collStats)));
        allBenchmarks["heuristics"] = makeVector<std::unique_ptr<Benchmark>>(
            std::make_unique<FullOptimizerBenchmark>(
                kCollName, kCollCardinality, makeHeuristicEstimatorFactoryFn()),
            std::make_unique<DeriveCEBenchmark>(
                kCollName, kCollCardinality, makeHeuristicEstimatorFactoryFn()));

        const std::string pipeline = "[{$match: " + query + "}]";

        generateHistorgrams(descriptor, collStats);

        BenchmarkResults results(std::move(descriptor));
        for (auto&& [estimationType, benchmarks] : allBenchmarks) {
            for (auto&& benchmark : benchmarks) {
                benchmark->setIndexes(results.getDescriptor().indexes);
                benchmark->runBenchmark(pipeline, results.getDescriptor().numIterations);
                results.addTimeMetrics(estimationType, benchmark->extractTimeMetrics());
            }
        }
        _resultAggregator->addResults(results);
    }

    static std::unique_ptr<BenchmarkResultsAggregator> _resultAggregator;
};

// Static initalizer for the '_resultAggregator'.
std::unique_ptr<BenchmarkResultsAggregator> CEBenchmarkTest::_resultAggregator =
    std::make_unique<BSONBenchmarkResultsAggregator>();

// Just a handy alias for the TEST_F macro.
#define BENCHMARK(BENCHMARK_NAME) TEST_F(CEBenchmarkTest, BENCHMARK_NAME)

//
// Start of benchmarks.
//

BENCHMARK(BucketSmallNumberSmallSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kSmallBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: 1}}");
}

BENCHMARK(BucketSmallNumberMediumSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kSmallBucketCounts,
         {{"a", BucketValueType::SmallStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: \"" + kSmallStr + "\"}}");
}

BENCHMARK(BucketSmallNumberLargeSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kSmallBucketCounts,
         {{"a", BucketValueType::BigStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: \"" + kBigStr + "\"}}");
}

BENCHMARK(BucketSmallNumberMixedSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kSmallBucketCounts,
         {{"a", BucketValueType::MixedIntStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: 1}}");
}

BENCHMARK(BucketMediumNumberSmallSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kMediumBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: 1}}");
}

BENCHMARK(BucketMediumNumberMediumSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kMediumBucketCounts,
         {{"a", BucketValueType::SmallStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: \"" + kSmallStr + "\"}}");
}

BENCHMARK(BucketMediumNumberLargeSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kMediumBucketCounts,
         {{"a", BucketValueType::BigStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: \"" + kBigStr + "\"}}");
}

BENCHMARK(BucketMediumNumberMixedSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kMediumBucketCounts,
         {{"a", BucketValueType::MixedIntStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: 1}}");
}

BENCHMARK(BucketLargeNumberSmallSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: 1}}");
}

BENCHMARK(BucketLargeNumberMediumSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::SmallStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: \"" + kSmallStr + "\"}}");
}

BENCHMARK(BucketLargeNumberLargeSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::BigStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: \"" + kBigStr + "\"}}");
}

BENCHMARK(BucketLargeNumberMixedSize) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::MixedIntStr}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gt: 1}}");
}

BENCHMARK(ArrayHistogramSmallBucketNumberSmallSize) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kSmallBucketCounts,
                   {{"a", BucketValueType::Int}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$gt: 300}}");
}

BENCHMARK(ArrayHistogramMediumBucketNumberSmallSize) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kMediumBucketCounts,
                   {{"a", BucketValueType::Int}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$gt: 300}}");
}

BENCHMARK(ArrayHistogramLargeBucketNumberSmallSize) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kLargeBucketCounts,
                   {{"a", BucketValueType::Int}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$gt: 300}}");
}

BENCHMARK(ArrayHistogramSmallBucketNumberMixedSize) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kSmallBucketCounts,
                   {{"a", BucketValueType::MixedIntStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$gt: 300}}");
}

BENCHMARK(ArrayHistogramMediumBucketNumberMixedSize) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kMediumBucketCounts,
                   {{"a", BucketValueType::MixedIntStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$gt: 300}}");
}

BENCHMARK(ArrayHistogramLargeBucketNumberMixedSize) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kLargeBucketCounts,
                   {{"a", BucketValueType::MixedIntStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$gt: 300}}");
}

BENCHMARK(ArrayHistogramSmallBucketNumberMixedSizeElemMatch) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kSmallBucketCounts,
                   {{"a", BucketValueType::MixedIntStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$elemMatch: {$gt: 20, $lt: 100}}}");
}

BENCHMARK(ArrayHistogramMediumBucketNumberMixedSizeElemMatch) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kMediumBucketCounts,
                   {{"a", BucketValueType::MixedIntStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$elemMatch: {$gt: 20, $lt: 100}}}");
}

BENCHMARK(ArrayHistogramLargeBucketNumberMixedSizeElemMatch) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kLargeBucketCounts,
                   {{"a", BucketValueType::MixedIntStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}},
                   1 /*numPredicates*/},
                  "{a: {$elemMatch: {$gt: 20, $lt: 100}}}");
}

BENCHMARK(ArrayHistogramSmallBucketNumberMixedSizeElemMatchConjunction) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kSmallBucketCounts,
                   {{"a", BucketValueType::Int}, {"b", BucketValueType::SmallStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)},
                    {"b_1", makeIndexDefinition("b", CollationOp::Ascending, true /*isMultiKey*/)}},
                   2 /*numPredicates*/},
                  "{a: {$elemMatch: {$gt: 20, $lt: 100}}, b: {$elemMatch: {$gt: \"" + kSmallStr +
                      "\"}}}");
}

BENCHMARK(ArrayHistogramMediumBucketNumberMixedSizeElemMatchConjunction) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kMediumBucketCounts,
                   {{"a", BucketValueType::Int}, {"b", BucketValueType::SmallStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)},
                    {"b_1", makeIndexDefinition("b", CollationOp::Ascending, true /*isMultiKey*/)}},
                   2 /*numPredicates*/},
                  "{a: {$elemMatch: {$gt: 20, $lt: 100}}, b: {$elemMatch: {$gt: \"" + kSmallStr +
                      "\"}}}");
}

BENCHMARK(ArrayHistogramLargeBucketNumberMixedSizeElemMatchConjunction) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kLargeBucketCounts,
                   {{"a", BucketValueType::Int}, {"b", BucketValueType::SmallStr}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)},
                    {"b_1", makeIndexDefinition("b", CollationOp::Ascending, true /*isMultiKey*/)}},
                   2 /*numPredicates*/},
                  "{a: {$elemMatch: {$gt: 20, $lt: 100}}, b: {$elemMatch: {$gt: \"" + kSmallStr +
                      "\"}}}");
}

BENCHMARK(BucketLargeNumber2HistogramsConjunctions) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}, {"b", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
          {"b_1", makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)}},
         2 /*numPredicates*/},
        "{a: {$gt: 1}, b: {$gt: 10}}");
}

BENCHMARK(BucketLargeNumber3HistogramsConjunctions) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}, {"b", BucketValueType::Int}, {"c", BucketValueType::Int}},
         {
             {"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
             {"b_1", makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)},
             {"c_1", makeIndexDefinition("c", CollationOp::Ascending, false /*isMultiKey*/)},
         },

         3 /*numPredicates*/},
        "{a: {$gt: 1}, b: {$gt: 10}, c: {$gt: 0}}");
}

BENCHMARK(BucketLargeNumber10HistogramsLargeConjunctions) {
    runBenchmarks(
        {_testInfo.testName(),
         1, /* numIterations, this test is slow so it's different from the default value */
         kLargeBucketCounts,
         {{"a", BucketValueType::Int},
          {"b", BucketValueType::Int},
          {"c", BucketValueType::Int},
          {"d", BucketValueType::Int}},
         {
             {"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
             {"b_1", makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)},
             {"c_1", makeIndexDefinition("c", CollationOp::Ascending, false /*isMultiKey*/)},
             {"d_1", makeIndexDefinition("d", CollationOp::Ascending, true /*isMultiKey*/)},
             {"e_1", makeIndexDefinition("e", CollationOp::Ascending, false /*isMultiKey*/)},
             {"f_1", makeIndexDefinition("f", CollationOp::Ascending, false /*isMultiKey*/)},
             {"g_1", makeIndexDefinition("g", CollationOp::Ascending, false /*isMultiKey*/)},
             {"h_1", makeIndexDefinition("h", CollationOp::Ascending, false /*isMultiKey*/)},
             {"i_1", makeIndexDefinition("i", CollationOp::Ascending, false /*isMultiKey*/)},
             {"j_1", makeIndexDefinition("j", CollationOp::Ascending, false /*isMultiKey*/)},
         },
         10 /*numPredicates*/},
        "{a: {$gt: 1}, b: {$gt: 10}, c: {$gt: 0}, d: {$elemMatch: {$gt: 10, $lt: 100}}, e: {$lte: "
        "80}, f: {$gte: 500}, g: {$eq: 79}, h: {$lt: 11}, i: {gt: 120}, j: {$eq: 44} }");
}

BENCHMARK(BucketLargeNumber2HistogramsDisjunctions) {
    runBenchmarks({_testInfo.testName(),
                   kNumIterations,
                   kLargeBucketCounts,
                   {{"a", BucketValueType::Int}, {"b", BucketValueType::Int}},
                   {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)},
                    {"b_1", makeIndexDefinition("b", CollationOp::Ascending, true /*isMultiKey*/)}},
                   2 /*numPredicates*/},
                  "{$or: [{a: {$gt: 1}}, {b: {$gt: 10}}]}");
}

BENCHMARK(BucketLargeNumber3HistogramsDisjunctions) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}, {"b", BucketValueType::Int}, {"c", BucketValueType::Int}},
         {
             {"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
             {"b_1", makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)},
             {"c_1", makeIndexDefinition("c", CollationOp::Ascending, false /*isMultiKey*/)},
         },
         3 /*numPredicates*/},
        "{$or: [{a: {$gt: 1}}, {b: {$gt: 10}}, {c: {$gt: 0}}]}");
}

BENCHMARK(BucketLargeNumberNestedConjunctionsDepth2) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}, {"b", BucketValueType::Int}, {"c", BucketValueType::Int}},
         {
             {"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
             {"b_1", makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)},
             {"c_1", makeIndexDefinition("c", CollationOp::Ascending, false /*isMultiKey*/)},
         },
         3 /*numPredicates*/},
        "{$and: [{$or: [{a: {$gt: 1}}, {b: {$gt: 10}}]}, {c: {$gt: 0}}]}");
}

BENCHMARK(BucketLargeNumberNestedConjunctionsDepth3) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int},
          {"b", BucketValueType::Int},
          {"c", BucketValueType::Int},
          {"d", BucketValueType::Int}},
         {
             {"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
             {"b_1", makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)},
             {"c_1", makeIndexDefinition("c", CollationOp::Ascending, false /*isMultiKey*/)},
             {"d_1", makeIndexDefinition("d", CollationOp::Ascending, false /*isMultiKey*/)},
         },

         4 /*numPredicates*/},
        "{$and: [{$or: [{a: {$gt: 1}}, {b: {$gt: 1}, c: {$gt: 10}}]}, {d: {$gt: 0}}]}");
}

BENCHMARK(BucketLargeNumberNestedDisjunctionsDepth2) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}, {"b", BucketValueType::Int}, {"c", BucketValueType::Int}},
         {
             {"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
             {"b_1", makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)},
             {"c_1", makeIndexDefinition("c", CollationOp::Ascending, false /*isMultiKey*/)},
         },
         3 /*numPredicates*/},
        "{$or: [{a: {$gt: 1}}, {$and: [{b: {$gt: 10}}, {c: {$gt: 0}}]}]}");
}

BENCHMARK(BucketLargeNumberNestedDisjunctionsDepth3) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int},
          {"b", BucketValueType::Int},
          {"c", BucketValueType::Int},
          {"d", BucketValueType::Int}},
         {
             {"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
             {"b_1", makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)},
             {"c_1", makeIndexDefinition("c", CollationOp::Ascending, false /*isMultiKey*/)},
             {"d_1", makeIndexDefinition("d", CollationOp::Ascending, false /*isMultiKey*/)},
         },

         4 /*numPredicates*/},
        "{$or: [ {a: {$gt: 1}}, {$and: [ {b: {$gt: 10}}, {$or: [ {c: {$gt: 0}}, {d: "
        "{$gt: 1}}]}]}]}");
}

BENCHMARK(BucketMediumNumberSmallSizeEQ) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kMediumBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$eq: 1}}");
}

BENCHMARK(BucketMediumNumberSmallSizeLT) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kMediumBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$lt: 90}}");
}

BENCHMARK(BucketMediumNumberSmallSizeLTE) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kMediumBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$lte: 90}}");
}

BENCHMARK(BucketMediumNumberSmallSizeGTE) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kMediumBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        "{a: {$gte: 90}}");
}

BENCHMARK(BucketLargeNumberSmallSizeGTBeginning) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        str::stream() << "{a: {$gt: " << kMinIntValue << "}}");
}

BENCHMARK(BucketLargeNumberSmallSizeGTMiddle) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        str::stream() << "{a: {$gt: " << kMiddleIntValue << "}}");
}

BENCHMARK(BucketLargeNumberSmallSizeGTEnd) {
    runBenchmarks(
        {_testInfo.testName(),
         kNumIterations,
         kLargeBucketCounts,
         {{"a", BucketValueType::Int}},
         {{"a_1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}},
         1 /*numPredicates*/},
        str::stream() << "{a: {$gt: " << kMaxIntValue << "}}");
}

// Must be the last test to run in this suite.
BENCHMARK(PrintResults) {
    _resultAggregator->printResults();
}

}  // namespace
}  // namespace mongo::optimizer::ce
