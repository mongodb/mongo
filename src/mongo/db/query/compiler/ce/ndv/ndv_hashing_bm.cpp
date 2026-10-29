// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <string>

#include <benchmark/benchmark.h>

namespace mongo::ce {
namespace {

// Runs once per document during an analyze scan; per-value cost (including the KeyString
// builder construction) has to stay negligible next to reading the document.

void BM_ndvHashInt(benchmark::State& state) {
    const BSONObj doc = BSON("a" << 12345);
    const BSONElement element = doc.firstElement();
    for (auto _ : state) {
        benchmark::DoNotOptimize(hashValueForNdv(element));
    }
}
BENCHMARK(BM_ndvHashInt);

void BM_ndvHashString(benchmark::State& state) {
    const BSONObj doc = BSON("a" << std::string(32, 'x'));
    const BSONElement element = doc.firstElement();
    for (auto _ : state) {
        benchmark::DoNotOptimize(hashValueForNdv(element));
    }
}
BENCHMARK(BM_ndvHashString);

void BM_ndvHashNestedObject(benchmark::State& state) {
    const BSONObj doc = BSON("a" << BSON("b" << BSON("c" << 1 << "d" << "xyz")));
    const BSONElement element = doc.firstElement();
    for (auto _ : state) {
        benchmark::DoNotOptimize(hashValueForNdv(element));
    }
}
BENCHMARK(BM_ndvHashNestedObject);

void BM_ndvHashWideObject(benchmark::State& state) {
    BSONObjBuilder wide;
    for (int i = 0; i < 100; ++i) {
        wide.append("field" + std::to_string(i), i);
    }
    const BSONObj doc = BSON("a" << wide.obj());
    const BSONElement element = doc.firstElement();
    for (auto _ : state) {
        benchmark::DoNotOptimize(hashValueForNdv(element));
    }
}
BENCHMARK(BM_ndvHashWideObject);

void BM_ndvHashDeeplyNestedObject(benchmark::State& state) {
    BSONObj nested = BSON("v" << 1);
    for (int i = 0; i < 30; ++i) {
        nested = BSON("v" << nested);
    }
    const BSONObj doc = BSON("a" << nested);
    const BSONElement element = doc.firstElement();
    for (auto _ : state) {
        benchmark::DoNotOptimize(hashValueForNdv(element));
    }
}
BENCHMARK(BM_ndvHashDeeplyNestedObject);

}  // namespace
}  // namespace mongo::ce
