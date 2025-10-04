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
