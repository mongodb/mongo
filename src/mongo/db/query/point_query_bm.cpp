// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_bm_fixture.h"
#include "mongo/util/processinfo.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class PointQueryBenchmark : public QueryBenchmarkFixture {
private:
    BSONObj generateDocument(size_t index, size_t approximateSize) override {
        std::string str;
        str.reserve(approximateSize);
        for (size_t i = 0; i < approximateSize; ++i) {
            str.push_back(randomLowercaseAlpha());
        }
        auto uniqueField = static_cast<long long>(index);
        auto nonUniqueField = static_cast<long long>(index / 2);
        return BSONObjBuilder{}
            .append("_id", OID::gen())
            .append("uniqueField", uniqueField)
            .append("nonUniqueField", nonUniqueField)
            .append("arrayField", std::vector<long long>{uniqueField, nonUniqueField})
            .append("str", str)
            .obj();
    }

    std::vector<BSONObj> getIndexSpecs() const override {
        return {buildIndexSpec("uniqueField", true),
                buildIndexSpec("nonUniqueField", false),
                buildIndexSpec("arrayField", false)};
    }
};

BENCHMARK_DEFINE_F(PointQueryBenchmark, IdPointQuery)
(benchmark::State& state) {
    auto id = docs()[docs().size() / 2].getField("_id").OID();
    runBenchmark(BSON("_id" << id), BSONObj{} /*projection*/, state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, UniqueFieldPointQuery)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 2;
    runBenchmark(BSON("uniqueField" << fieldValue), BSONObj{} /*projection*/, state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, NonUniqueFieldPointQuery)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 3;
    runBenchmark(BSON("nonUniqueField" << fieldValue), BSONObj{} /*projection*/, state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, ArrayFieldPointQuery)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 3;
    runBenchmark(BSON("arrayField" << fieldValue), BSONObj{} /*projection*/, state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, UniqueFieldPointQueryWithCoveredProjection)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 2;
    runBenchmark(BSON("uniqueField" << fieldValue), BSON("_id" << 0 << "uniqueField" << 1), state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, UniqueFieldPointQueryWithNotCoveredProjection)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 2;
    runBenchmark(
        BSON("uniqueField" << fieldValue), BSON("_id" << 0 << "nonUniqueField" << 1), state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, NonUniqueFieldPointQueryWithCoveredProjection)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 3;
    runBenchmark(
        BSON("nonUniqueField" << fieldValue), BSON("_id" << 0 << "nonUniqueField" << 1), state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, NonUniqueFieldPointQueryWithNotCoveredProjection)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 3;
    runBenchmark(
        BSON("nonUniqueField" << fieldValue), BSON("_id" << 0 << "uniqueField" << 1), state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, IdPointQueryWithCoveredProjection)
(benchmark::State& state) {
    auto id = docs()[docs().size() / 2].getField("_id").OID();
    runBenchmark(BSON("_id" << id), BSON("_id" << 1), state);
}


BENCHMARK_DEFINE_F(PointQueryBenchmark, IdPointQueryWithNotCoveredProjection)
(benchmark::State& state) {
    auto id = docs()[docs().size() / 2].getField("_id").OID();
    runBenchmark(BSON("_id" << id), BSON("_id" << 0 << "uniqueField" << 1), state);
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

static void configureProjectionBenchmarks(benchmark::internal::Benchmark* bm) {
    // Varying document size allows us to measure the effect of covering the projection with an
    // index.
    bm->ThreadRange(1, kMaxThreads)->Args({10, 256 * 1024})->Args({10, 4096 * 1024});
}

BENCHMARK_REGISTER_F(PointQueryBenchmark, IdPointQuery)->Apply(configureBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, UniqueFieldPointQuery)->Apply(configureBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, NonUniqueFieldPointQuery)->Apply(configureBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, ArrayFieldPointQuery)->Apply(configureBenchmarks);

BENCHMARK_REGISTER_F(PointQueryBenchmark, UniqueFieldPointQueryWithCoveredProjection)
    ->Apply(configureProjectionBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, UniqueFieldPointQueryWithNotCoveredProjection)
    ->Apply(configureProjectionBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, NonUniqueFieldPointQueryWithCoveredProjection)
    ->Apply(configureProjectionBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, NonUniqueFieldPointQueryWithNotCoveredProjection)
    ->Apply(configureProjectionBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, IdPointQueryWithCoveredProjection)
    ->Apply(configureProjectionBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, IdPointQueryWithNotCoveredProjection)
    ->Apply(configureProjectionBenchmarks);

}  // namespace
}  // namespace mongo
