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

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/query/ce/heuristic_estimator.h"
#include "mongo/db/query/ce/histogram_estimator.h"
#include "mongo/db/query/ce/test_utils.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"
#include "mongo/db/query/stats/collection_statistics_mock.h"
#include "mongo/db/query/util/named_enum.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_tick_source.h"

namespace mongo::optimizer::ce {
/**
 * A callback function to return a 'CardinalityEstimator' of a specific type. Used with the
 * 'Benchmark' class implementations to specify which estimation method to benchmark.
 */
using CardinalityEstimatorFactoryFn =
    std::function<std::unique_ptr<cascades::CardinalityEstimator>()>;

/**
 * A map between measured metrics (such as "optimize" method or "deriveCE") and a vector of time
 * series holding elapsed execution time for each metric. A benchmark is executed a number of
 * iterations, and during each iteration each metric could be collected multiple times (as a
 * corresponding method could be invoked multiple times). For instance, if the number of iterations
 * was 2, and "deriveCE" method was called 3 times, then one entry in this map might look as
 follows:

 *    "deriveCE" => [12, 11, 10, 12, 14, 16]
 */
using TimeMetrics = std::map<std::string, std::vector<Nanoseconds>>;

/**
 * When generating histograms for a benchmark, this is used to define which type of data be to put
 * into histogram buckets.
 */
#define BUCKET_VALUE_TYPES_TABLE(X) \
    X(Int)                          \
    X(SmallStr)                     \
    X(BigStr)                       \
    X(MixedIntStr)

QUERY_UTIL_NAMED_ENUM_DEFINE(BucketValueType, BUCKET_VALUE_TYPES_TABLE)
#undef BUCKET_VALUE_TYPES_TABLE

/**
 * A utility function to return a string holding a name of the given ABT node.
 */
inline std::string abtNodeName(ABT::reference_type abt) {
    auto explain = ExplainGenerator::explainBSONObj(abt);
    return explain["nodeType"].String();
}

/**
 * Returns a factory function to create a histograms-based estimator using a provided collection
 * statistics.
 */
inline CardinalityEstimatorFactoryFn makeHistogramEstimatorFactoryFn(
    std::shared_ptr<stats::CollectionStatistics> collStats) {
    return [collStats]() {
        return std::make_unique<HistogramEstimator>(collStats, makeHeuristicCE());
    };
}

/**
 * Returns a factory function to create a heuristics based estimator.
 */
inline CardinalityEstimatorFactoryFn makeHeuristicEstimatorFactoryFn() {
    return []() { return makeHeuristicCE(); };
}

/**
 * Contains various parameters describing the setup under which a particular benchmark is to be
 * executed. Used to report the results of the benchmark.
 */
struct BenchmarkDescriptor {
    const StringData benchmarkName;

    /**
     * The number of times to repeat the benchmark.
     */
    const size_t numIterations;

    /**
     * The number of buckets per a generated  histogram.
     */
    const size_t numBuckets;

    /**
     * A map between field paths and value types enum indicating which values to be put in a
     * histogram for this path.
     */
    const opt::unordered_map<std::string, BucketValueType> valueTypes;

    /**
     * A collection metadata describing indexes defined on the collection. Stored as a map between
     * an index name and 'IndexDefinition' object.
     */
    const opt::unordered_map<std::string, IndexDefinition> indexes;

    /**
     * The number of individual (atom) predicates in a benchmark query.
     */
    const size_t numPredicates;
};

/**
 * This class holds the results of an execution of a single CE benchmark. Given a query and certain
 * input parameters (such as collection statistics) a typical benchmark will run a number of
 * experiments to measure execution time (or time metrics) across different optimizer phases, using
 * different estimation methods, using different granularity (e.g., total time vs time per specific
 * ABT node). All collected data will be stored in this result class.
 *
 * Here is an example of what it may look like:
 *
 *      benchmarkName: "BucketSmallNumberSmallSize"
 *      numIterations: 100
 *      results:
 *          "heuristics":
 *               "optimize": [1200, 1215, 1210, ...]
 *               "deriveCE": [200, 210, 215, 210, 211, ...]
 *               "deriveCE_Sargable": [1000, 950, 1100, ...]
 *               "deriveCE_Scan": [100, 120, 115, ...]
 *          "histograms":
 *               "optimize": [1300, 1315, 1310, ...]
 *               "deriveCE": [300, 310, 315, 310, 311, ...]
 *               "deriveCE_Sargable": [1100, 990, 1150, ...]
 *               "deriveCE_Scan": [200, 220, 215, ...]
 *
 * The class also provides methods to serialize aggregated data into different formats, such as
 * plain string or BSON. For each metric under each estimation method the output will contain the
 * following aggregated data:
 *
 *          "heuristics":
 *                "optimize":
 *                      "totalTime": 445565
 *                      "minTime": 4427
 *                      "maxTime": 4758
 *                      "avgTime": 4455
 *                      "calls": 100
 *                "deriveCE":
 *                      "totalTime": 25098
 *                      "minTime": 13
 *                      "maxTime": 86
 *                      "avgTime": 35
 *                      "calls": 700
 *
 * Where,
 *    - totalTime - total execution time for a metric (method) across all iterations
 *    - minTime - min execution time for a metric across all iterations
 *    - maxTime - max execution time for a specific metric across all iterations
 *    - avgTime - average execution time for a specific metric across all iterations (totalTme /
 *                calls)
 *    - calls - a total number of calls of a specific method across all iterations. E.g., if
 *              the number of iterations per benchmark was 100 and in each iteration the
 *              "deriveCE_Scan" method was called 7 times, the reported "calls" metric will be 700.
 *
 * Note that presently we do not distinguish between different instances of ABT types. For instance,
 * if there were two Scan nodes in the plan, their data points will be collected under a single
 * "deriveCE_Scan" metric.
 */
class BenchmarkResults {
public:
    BenchmarkResults(BenchmarkDescriptor descriptor) : _descriptor(std::move(descriptor)) {}

    const BenchmarkDescriptor& getDescriptor() const {
        return _descriptor;
    }

    /**
     * Adds time metrics for the given cardinally 'estimationType' to this results instance.
     * This method can be called multiple times during execution of a particular CE benchmark,
     * so the results for each 'estimationType' are merged together. For instance, a benchmark
     * could collected time metrics for the "optimize" methods of the "heuristics" estimation
     * and put them into a 'BenchmarkResults' object, and then run another experiment to collect
     * various "deriveCE" metrics for the same estimation method. When 'addTimeMetrics' is called
     * for the second time, the result is merged with the previously stored metrics for the
     * "heuristics" estimation type.
     */
    void addTimeMetrics(const std::string& estimationType, TimeMetrics metrics) {
        _results[estimationType].merge(std::move(metrics));
    }

    /**
     * Serializes aggregated results of the benchmark into a BSON object.
     */
    BSONObj toBSON() const;

    /**
     * Serializes aggregated results of the benchmark into a plan string.
     */
    std::string toString() const;

private:
    using DurationType = decltype(durationCount<Microseconds>(Nanoseconds(0)));

    /**
     * The returned DurationType values correspond to total, min, max, avg times aggregated from
     * the given 'times' array, with the last parameter being the number of calls performed to
     * collect the times.
     */
    std::tuple<DurationType, DurationType, DurationType, DurationType, long long>
    aggregateTimeMetrics(std::vector<Nanoseconds> times) const;

    const BenchmarkDescriptor _descriptor;
    std::map<std::string, TimeMetrics> _results;
};

/**
 * This interface defines an aggregator for 'BenchmarkResults'. Each CE benchmark will produce a
 * 'BenchmarkResults' holding the results of the experiment. Rather than dumping this result
 * straight into console, we will store it within a specific 'BenchmarkResultsAggregator' and output
 * all results at once at the end of the test in a configured format. A deferred output will not
 * interfere with the output of the test framework, making it easier to interpret the results
 * without test noise.
 */
class BenchmarkResultsAggregator {
public:
    virtual ~BenchmarkResultsAggregator() = default;

    virtual void addResults(const BenchmarkResults& results) = 0;

    virtual void printResults() = 0;
};

/**
 * A benchmark results aggregator which wil output the results as a BSON array of serialized
 * 'BenchmarkResults' objects.
 */
class BSONBenchmarkResultsAggregator : public BenchmarkResultsAggregator {
public:
    void addResults(const BenchmarkResults& results) override {
        _arrayBuilder.append(results.toBSON());
    }

    void printResults() {
        std::cout << _arrayBuilder.arr().jsonString(
                         ExtendedCanonicalV2_0_0, 1 /*pretty*/, true /*isArray*/)
                  << std::endl;
    }

private:
    BSONArrayBuilder _arrayBuilder;
};

/**
 * A benchmark results aggregator which wil output the results as a plain text. The results of each
 * benchmark will output on a new line.
 */
class StringBenchmarkResultsAggregator : public BenchmarkResultsAggregator {
public:
    void addResults(const BenchmarkResults& results) override {
        _results.push_back(results.toString());
    }

    void printResults() {
        for (auto&& str : _results) {
            std::cout << str << std::endl;
        }
    }

private:
    std::vector<std::string> _results;
};

/**
 * A base class to implement a CE benchmark. It provides a 'runBenchmark' method which takes a
 * pipeline (representing as a raw string) and a 'numIterations' parameter to indicate how many
 * times to run the benchmark. On each iteration the pipeline is translated into an ABT and
 * 'CETester::getCE' is called. Specially crafted CE benchmarks will measure execution of different
 * phases of the optimizer and collect metrics. This class provides helper utilities to obtain
 * a 'ScopedTimer' and collect metrics within a 'TimeMetrics' instances, which can be obtained once
 * the benchmark is completed.
 *
 * Note that CE benchmarks are implemented using an ad-hoc benchmark facility, rather than the
 * standard Google benchmark framework. This was done so as Google benchmark doesn't fit for purpose
 * in this particular case, since it was designed to measure an overall execution time of a
 * particular function. So, it would suite fine if we wanted to benchmark just the
 * 'OptPhaseManager::optimize' call. However, we're also interested in benchmarking just the CE
 * module which is invoked many times during 'optimize' without including any other overhead, so
 * we had to hand craft this framework for CE benchmarks.
 */
class Benchmark : public CETester {
public:
    using CETester::CETester;

    /**
     * Executes 'getCE' method 'numIterations' number of times on an ABT translated from the
     * given pipeline. Collected time merics can be obtained with 'extractTimeMetrics' at the
     * end of the benchmark run.
     */
    void runBenchmark(const std::string& pipeline, size_t numIterations);

    /**
     * Returns time metrics collected during the benchmark run. This is a destructive method as
     * the metrics are extracted (moved) from the internal state of this instance.
     */
    TimeMetrics extractTimeMetrics() {
        return std::move(_timeMetrics);
    }

protected:
    ScopedTimer getTimer(Nanoseconds* counter) const {
        return {counter, _tickSource.get()};
    }

    void collectElapsedTime(const std::string& metricName, Nanoseconds elapsed) const {
        _timeMetrics[metricName].push_back(elapsed);
    }

private:
    mutable TimeMetrics _timeMetrics;
    const std::unique_ptr<TickSource> _tickSource = makeSystemTickSource();

    friend class CardinalityEstimatorWrapper;
};

/**
 * This CardinalityEstimator implementation is a wrapper around a specific CardinalityEstimator
 * passed to this object during construction. Its purpose is to override the 'deriveCE' method
 * to measure execution time of the wrapped estimator and save collected time metrics within the
 * provided 'Benchmark' instance.
 */
class CardinalityEstimatorWrapper : public cascades::CardinalityEstimator {
public:
    CardinalityEstimatorWrapper(const Benchmark* benchmark,
                                std::unique_ptr<cascades::CardinalityEstimator> estimator)
        : _benchmark(benchmark), _estimator(std::move(estimator)) {}

    ~CardinalityEstimatorWrapper() override {
        // Collect total 'deriveCE' time.
        _benchmark->collectElapsedTime("deriveCE", _elapsedTimePerQuery);
    }

    CEType deriveCE(const Metadata& metadata,
                    const cascades::Memo& memo,
                    const properties::LogicalProps& logicalProps,
                    ABT::reference_type logicalNodeRef) const final;

private:
    const Benchmark* _benchmark;
    std::unique_ptr<cascades::CardinalityEstimator> _estimator;
    mutable Nanoseconds _elapsedTimePerQuery{0};
};

/**
 * A CE benchmark designed to measure execution time of the 'OptPhaseManager::optimize' method.
 * A cardinally estimation method to be used during this benchmark is specified with the
 * 'CardinalityEstimatorFactoryFn' passed to the constructor.
 */
class FullOptimizerBenchmark : public Benchmark {
public:
    FullOptimizerBenchmark(const std::string& collName,
                           CEType collCardinality,
                           CardinalityEstimatorFactoryFn createEstimatorFn)
        : Benchmark(collName, collCardinality), _createEstimatorFn(std::move(createEstimatorFn)) {}

protected:
    void optimize(OptPhaseManager& phaseManager, ABT& abt) const override;

    std::unique_ptr<cascades::CardinalityEstimator> getEstimator(
        bool /*forValidation*/) const final {
        return _createEstimatorFn();
    }

private:
    CardinalityEstimatorFactoryFn _createEstimatorFn;
};

/**
 * A CE benchmark designed to measure execution time spent in CE module. A cardinally estimation
 * method to be used during this benchmark is specified with the 'CardinalityEstimatorFactoryFn'
 * passed to the constructor.
 */
class DeriveCEBenchmark : public Benchmark {
public:
    DeriveCEBenchmark(const std::string& collName,
                      CEType collCardinality,
                      CardinalityEstimatorFactoryFn createEstimatorFn)
        : Benchmark(collName, collCardinality), _createEstimatorFn(std::move(createEstimatorFn)) {}

protected:
    std::unique_ptr<cascades::CardinalityEstimator> getEstimator(
        bool forValidation) const override {
        return forValidation
            ? _createEstimatorFn()
            : std::make_unique<CardinalityEstimatorWrapper>(this, _createEstimatorFn());
    }

private:
    CardinalityEstimatorFactoryFn _createEstimatorFn;
};
}  // namespace mongo::optimizer::ce
