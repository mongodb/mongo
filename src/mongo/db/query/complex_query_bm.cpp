// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_bm_fixture.h"
#include "mongo/util/processinfo.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class ComplexQueryBenchmark : public QueryBenchmarkFixture {
private:
    BSONObj generateDocument(size_t index, size_t approximateSize) override {
        BSONArrayBuilder array2d;
        for (size_t i = 0; i < approximateSize; ++i) {
            BSONArrayBuilder subArray;
            for (size_t j = 0; j <= i; ++j) {
                subArray.append(randomInt64());
            }
            array2d.append(subArray.arr());
        }

        return BSONObjBuilder{}
            .append("_id", OID::gen())
            .append("index", static_cast<int64_t>(index))
            .append("array2d", array2d.arr())
            .obj();
    }

    std::vector<BSONObj> getIndexSpecs() const override {
        return {buildIndexSpec("index", true)};
    }
};

BENCHMARK_DEFINE_F(ComplexQueryBenchmark, ComplexQueryComplexProjection)
(benchmark::State& state) {
    runBenchmark(query_benchmark_constants::kComplexPredicate,
                 query_benchmark_constants::kComplexProjection,
                 state);
}

BENCHMARK_DEFINE_F(ComplexQueryBenchmark, SimpleQueryComplexProjection)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 2;
    runBenchmark(BSON("index" << fieldValue), query_benchmark_constants::kComplexProjection, state);
}

BENCHMARK_DEFINE_F(ComplexQueryBenchmark, SimpleQueryComplexProjectionNoDocuments)
(benchmark::State& state) {
    runBenchmark(BSON("index" << -1), query_benchmark_constants::kComplexProjection, state);
}

/**
 * ASAN can't handle the # of threads the benchmark creates. With sanitizers, run this in a
 * diminished "correctness check" mode. See SERVER-73168.
 */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
const auto kMaxThreads = 1;
#else
/** 2x to benchmark the case of more threads than cores for curiosity's sake. */
const auto kMaxThreads = 2 * ProcessInfo::getNumLogicalCores();
#endif

static void configureBenchmarks(benchmark::internal::Benchmark* bm) {
    bm->ThreadRange(1, kMaxThreads)->Args({10, 1024});
}

BENCHMARK_REGISTER_F(ComplexQueryBenchmark, ComplexQueryComplexProjection)
    ->Apply(configureBenchmarks);
BENCHMARK_REGISTER_F(ComplexQueryBenchmark, SimpleQueryComplexProjection)
    ->Apply(configureBenchmarks);
BENCHMARK_REGISTER_F(ComplexQueryBenchmark, SimpleQueryComplexProjectionNoDocuments)
    ->Apply(configureBenchmarks);
}  // namespace
}  // namespace mongo
