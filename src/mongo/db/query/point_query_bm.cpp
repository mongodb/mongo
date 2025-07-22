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

#include "mongo/db/query/query_bm_fixture.h"

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
