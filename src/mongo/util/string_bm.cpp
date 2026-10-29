// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/ctype.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cctype>  // NOLINT
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

// Verify the performance of our string processing algorithms.
// This can include std::string_view, util/str utilities, etc.

namespace mongo {
namespace {

std::string makeString(size_t size) {
    std::string_view fill = "The quick brown fox jumped over the lazy dog. ";
    std::string s;
    while (s.size() < size) {
        size_t avail = size - s.size();
        std::string_view fillSub = fill.substr(0, std::min(avail, fill.size()));
        s.append(fillSub.begin(), fillSub.end());
    }
    return s;
}

void BM_StringDataEqualCaseInsensitive(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    std::string s2 = s1;
    std::string_view sd1 = s1;
    for (auto _ : state) {
        benchmark::DoNotOptimize(str::equalCaseInsensitive(sd1, s2));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_StringDataEqualCaseInsensitive);

void BM_StdToLower(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    for (auto _ : state) {
        for (char& c : s1)
            benchmark::DoNotOptimize(c = std::tolower(c));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_StdToLower);

void BM_MongoCtypeToLower(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    for (auto _ : state) {
        for (char& c : s1)
            benchmark::DoNotOptimize(c = ctype::toLower(c));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_MongoCtypeToLower);

void BM_StdIsAlpha(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    for (auto _ : state) {
        for (char& c : s1)
            benchmark::DoNotOptimize(std::isalpha(c));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_StdIsAlpha);

void BM_MongoCtypeIsAlpha(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    for (auto _ : state) {
        for (char& c : s1)
            benchmark::DoNotOptimize(ctype::isAlpha(c));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_MongoCtypeIsAlpha);

}  // namespace
}  // namespace mongo
