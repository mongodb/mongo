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

#include "mongo/db/query/ce/benchmark_utils.h"

namespace mongo::optimizer::ce {
BSONObj BenchmarkResults::toBSON() const {
    BSONObjBuilder bob;
    bob.append("benchmarkName", _descriptor.benchmarkName)
        .appendNumber("iterations", static_cast<long long>(_descriptor.numIterations))
        .appendNumber("buckets", static_cast<long long>(_descriptor.numBuckets));

    BSONArrayBuilder valueTypesBab(bob.subarrayStart("valueTypes"));
    for (auto&& [fieldName, valueType] : _descriptor.valueTypes) {
        valueTypesBab.append(BSON(fieldName << toStringData(valueType)));
    }
    valueTypesBab.doneFast();

    BSONArrayBuilder indexesBab(bob.subarrayStart("indexes"));
    for (auto&& [indexName, indexDef] : _descriptor.indexes) {
        indexesBab.append(BSON(indexName << BSON("isMultiKey" << indexDef.isMultiKey())));
    }
    indexesBab.doneFast();

    bob.appendNumber("predicates", static_cast<long long>(_descriptor.numPredicates))
        .append("timeUnits", "microseconds");

    BSONObjBuilder resultsBob(bob.subobjStart("results"));
    for (auto&& [estimationType, timeMetrics] : _results) {
        BSONObjBuilder estimationTypeBob(resultsBob.subobjStart(estimationType));
        for (auto&& [metricName, times] : timeMetrics) {
            BSONObjBuilder metricBob(estimationTypeBob.subobjStart(metricName));

            auto [total, min, max, avg, calls] = aggregateTimeMetrics(times);

            metricBob.append("totalTime", total)
                .appendNumber("minTime", min)
                .appendNumber("maxTime", max)
                .appendNumber("avgTime", avg)
                .appendNumber("calls", calls);

            metricBob.doneFast();
        }
        estimationTypeBob.doneFast();
    }
    resultsBob.doneFast();

    return bob.obj();
}

std::string BenchmarkResults::toString() const {
    const std::string valueTypes = std::accumulate(
        _descriptor.valueTypes.begin(),
        _descriptor.valueTypes.end(),
        std::string{},
        [](std::string str, const std::pair<std::string, BucketValueType>& type) {
            return std::move(str) + "_" + type.first + "_" + toStringData(type.second);
        });
    const std::string prefix = str::stream()
        << _descriptor.benchmarkName << "_iterations_" << _descriptor.numIterations << "_buckets_"
        << _descriptor.numBuckets << "_types" << valueTypes << "_predicates_"
        << _descriptor.numPredicates;
    std::ostringstream ss;

    for (auto&& [estimationType, timeMetrics] : _results) {
        for (auto&& [metricName, times] : timeMetrics) {
            auto [total, min, max, avg, calls] = aggregateTimeMetrics(times);

            ss << prefix << "_" << estimationType << "_" << metricName << ": totalTime=" << total
               << "µs, minTime=" << min << "µs, maxTime=" << max << "µs, avgTime=" << avg
               << "µs, calls=" << calls << std::endl;
        }
    }

    return ss.str();
}

std::tuple<BenchmarkResults::DurationType,
           BenchmarkResults::DurationType,
           BenchmarkResults::DurationType,
           BenchmarkResults::DurationType,
           long long>
BenchmarkResults::aggregateTimeMetrics(std::vector<Nanoseconds> times) const {
    auto total =
        durationCount<Microseconds>(std::accumulate(times.begin(), times.end(), Nanoseconds(0)));
    auto min = durationCount<Microseconds>(*std::min_element(times.begin(), times.end()));
    auto max = durationCount<Microseconds>(*std::max_element(times.begin(), times.end()));
    auto avg = times.empty() ? 0.0 : static_cast<double>(total) / times.size();

    return {total, min, max, avg, times.size()};
}

void Benchmark::runBenchmark(const std::string& pipeline, size_t numIterations) {
    Metadata metadata = {{{getCollName(), createScanDef({}, {})}}};
    PrefixId prefixId = PrefixId::createForTests();
    ABT abt =
        translatePipeline(metadata, pipeline, prefixId.getNextId("scan"), getCollName(), prefixId);
    for (size_t iter = 0; iter < numIterations; ++iter) {
        ABT copy = abt;
        ASSERT_GTE(getCE(copy), 0);
    }
}

CEType CardinalityEstimatorWrapper::deriveCE(const Metadata& metadata,
                                             const cascades::Memo& memo,
                                             const properties::LogicalProps& logicalProps,
                                             ABT::reference_type logicalNodeRef) const {
    Nanoseconds elapsed(0);
    CEType ce = [&]() {
        auto timer = _benchmark->getTimer(&elapsed);
        return _estimator->deriveCE(metadata, memo, logicalProps, logicalNodeRef);
    }();

    // Accumulate total 'deriveCE' time per query.
    _elapsedTimePerQuery += elapsed;

    // Collect 'deriveCE' per ABT node. We currently do not distinguish between different
    // instances of the same ABT type.
    _benchmark->collectElapsedTime("deriveCE_" + abtNodeName(logicalNodeRef), elapsed);
    return ce;
}

void FullOptimizerBenchmark::optimize(OptPhaseManager& phaseManager, ABT& abt) const {
    Nanoseconds elapsed(0);
    {
        auto timer = getTimer(&elapsed);
        Benchmark::optimize(phaseManager, abt);
    }
    collectElapsedTime("optimize", elapsed);
}

}  // namespace mongo::optimizer::ce
