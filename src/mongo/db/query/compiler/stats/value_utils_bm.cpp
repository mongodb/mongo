// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/compiler/stats/value_utils.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/test_utils.h"

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo::stats {

enum class TypeTagsPair {
    All = 0,
    Shallow = 1,
    Heap = 2,
};

auto genAllPairs(TypeTagsPair option) {
    switch (option) {
        case TypeTagsPair::Shallow:
            return generateShallowTypeTagPairs();
        case TypeTagsPair::Heap:
            return generateHeapTypeTagPairs();
        case TypeTagsPair::All:
        default:
            return generateAllTypeTagPairs();
    }
}

void BM_sameTypeClass(benchmark::State& state) {
    auto allTypeTagPairs = genAllPairs(static_cast<TypeTagsPair>(state.range(0)));
    size_t i = 0;
    for (auto _ : state) {
        auto& typeTagPair = allTypeTagPairs[i];
        benchmark::DoNotOptimize(sameTypeClass(typeTagPair.first, typeTagPair.second));
        i = (i + 1) % allTypeTagPairs.size();
    }
}

void BM_sameTypeBracket(benchmark::State& state) {
    auto allTypeTagPairs = genAllPairs(static_cast<TypeTagsPair>(state.range(0)));
    size_t i = 0;
    for (auto _ : state) {
        auto& typeTagPair = allTypeTagPairs[i];
        benchmark::DoNotOptimize(sameTypeBracket(typeTagPair.first, typeTagPair.second));
        i = (i + 1) % allTypeTagPairs.size();
    }
}

void BM_sameTypeClassByComparingMin(benchmark::State& state) {
    auto allTypeTagPairs = genAllPairs(static_cast<TypeTagsPair>(state.range(0)));
    size_t i = 0;
    for (auto _ : state) {
        auto& typeTagPair = allTypeTagPairs[i];
        benchmark::DoNotOptimize(
            sameTypeClassByComparingMin(typeTagPair.first, typeTagPair.second));
        i = (i + 1) % allTypeTagPairs.size();
    }
}

void BM_isFullBracketInterval(benchmark::State& state) {
    auto start = sbe::value::makeNewObject();
    auto end = sbe::value::makeNewArray();
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            isFullBracketInterval(start.first, start.second, true, end.first, end.second, false));
    }
    sbe::value::ValueGuard startGuard{start}, endGuard{end};
}

void BM_bracketizeInterval(benchmark::State& state) {
    auto [startTag, startVal] = makeInt64Value(100);
    auto [endTag, endVal] = makeBooleanValue(1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            bracketizeInterval(startTag, startVal, true, endTag, endVal, false));
    }
}

void BM_bracketizeIntervalMinKeyToMaxKey(benchmark::State& state) {
    auto startTag = sbe::value::TypeTags::MinKey;
    auto startVal = sbe::value::Value(0);
    auto endTag = sbe::value::TypeTags::MaxKey;
    auto endVal = sbe::value::Value(0);
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            bracketizeInterval(startTag, startVal, true, endTag, endVal, false));
    }
}

BENCHMARK(BM_sameTypeClass)->Arg(0)->Arg(1)->Arg(2);
BENCHMARK(BM_sameTypeBracket)->Arg(0)->Arg(1)->Arg(2);
BENCHMARK(BM_sameTypeClassByComparingMin)->Arg(0)->Arg(1)->Arg(2);
BENCHMARK(BM_isFullBracketInterval);
BENCHMARK(BM_bracketizeInterval);
BENCHMARK(BM_bracketizeIntervalMinKeyToMaxKey);

}  // namespace mongo::stats
