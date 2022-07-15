/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <random>

#include "mongo/platform/basic.h"

#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"

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
    RecordId rid1(str1.c_str(), str1.size());
    RecordId rid2(str1.c_str(), str1.size());
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

    RecordId rid = RecordId(buf, bufLen);
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

    RecordId rid = RecordId(buf, bufLen);
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
    auto comp = [](const KV& left, const KV& right) { return left.key < right.key; };
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
