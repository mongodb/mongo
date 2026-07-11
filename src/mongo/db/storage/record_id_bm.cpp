// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/record_id.h"

#include "mongo/bson/oid.h"
#include "mongo/db/record_id_helpers.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

void BM_RecordIdCompareLong(benchmark::State& state) {
    RecordId rid(1 << 31);
    RecordId tmp(1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(rid > tmp);
        benchmark::ClobberMemory();
    }
}

void BM_RecordIdCompareSmallStr(benchmark::State& state) {
    std::string str1(20, 'x');
    RecordId rid1(str1);
    RecordId rid2(str1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(rid1 > rid2);
        benchmark::ClobberMemory();
    }
}

void BM_RecordIdIsValidLong(benchmark::State& state) {
    RecordId rid(1 << 31);
    for (auto _ : state) {
        benchmark::DoNotOptimize(rid.isValid());
        benchmark::ClobberMemory();
    }
}

void BM_RecordIdCopyLong(benchmark::State& state) {
    RecordId rid(1 << 31);
    for (auto _ : state) {
        RecordId tmp;
        benchmark::DoNotOptimize(tmp = rid);
        benchmark::ClobberMemory();
    }
}

void BM_RecordIdCopyOID(benchmark::State& state) {
    RecordId rid = record_id_helpers::keyForOID(OID::gen());
    for (auto _ : state) {
        RecordId tmp;
        benchmark::DoNotOptimize(tmp = rid);
        benchmark::ClobberMemory();
    }
}

void BM_RecordIdCopyMedString(benchmark::State& state) {
    constexpr int bufLen = 128;
    char buf[bufLen];
    memset(buf, 'x', bufLen);

    RecordId rid = RecordId(buf);
    for (auto _ : state) {
        RecordId tmp;
        benchmark::DoNotOptimize(tmp = rid);
        benchmark::ClobberMemory();
    }
}

void BM_RecordIdCopyBigString(benchmark::State& state) {
    constexpr int bufLen = 2048;
    char buf[bufLen];
    memset(buf, 'x', bufLen);

    RecordId rid = RecordId(buf);
    for (auto _ : state) {
        RecordId tmp;
        benchmark::DoNotOptimize(tmp = rid);
        benchmark::ClobberMemory();
    }
}

void BM_RecordIdFormatLong(benchmark::State& state) {
    RecordId rid(1 << 31);
    for (auto _ : state) {
        benchmark::DoNotOptimize(rid.withFormat([](RecordId::Null) { return false; },
                                                [](std::int64_t val) { return false; },
                                                [](const char* str, int size) { return false; }));
        benchmark::ClobberMemory();
    }
}

void BM_RecordIdFormatString(benchmark::State& state) {
    RecordId rid = record_id_helpers::keyForOID(OID::gen());
    for (auto _ : state) {
        benchmark::DoNotOptimize(rid.withFormat([](RecordId::Null) { return false; },
                                                [](std::int64_t val) { return false; },
                                                [](const char* str, int size) { return false; }));
        benchmark::ClobberMemory();
    }
}

template <typename V>
void BM_RecordIdSort(benchmark::State& state) {
    std::mt19937_64 gen(1234);
    std::uniform_int_distribution<uint64_t> dist;
    int64_t last = 0;
    struct KV {
        uint64_t key;
        V val;
    };
    auto comp = [](const KV& left, const KV& right) {
        return left.key < right.key;
    };
    std::vector<KV> data;

    for (auto j = 0; j < state.range(0); ++j)
        data.emplace_back(KV{dist(gen), V(++last)});
    for (auto _ : state) {
        auto copy = data;
        std::sort(copy.begin(), copy.end(), comp);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(data.size() * state.iterations());
    state.SetBytesProcessed(data.size() * state.iterations() * sizeof(KV));
}

BENCHMARK_TEMPLATE(BM_RecordIdSort, uint64_t)->RangeMultiplier(10)->Range(100, 100'000);
BENCHMARK_TEMPLATE(BM_RecordIdSort, RecordId)->RangeMultiplier(10)->Range(100, 100'000);

BENCHMARK(BM_RecordIdCopyLong);
BENCHMARK(BM_RecordIdCopyOID);
BENCHMARK(BM_RecordIdCopyMedString);
BENCHMARK(BM_RecordIdCopyBigString);

BENCHMARK(BM_RecordIdCompareLong);
BENCHMARK(BM_RecordIdCompareSmallStr);
BENCHMARK(BM_RecordIdIsValidLong);

BENCHMARK(BM_RecordIdFormatLong);
BENCHMARK(BM_RecordIdFormatString);

}  // namespace
}  // namespace mongo
