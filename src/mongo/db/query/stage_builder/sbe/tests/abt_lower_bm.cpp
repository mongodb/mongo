// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/abt_lower.h"

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"

#include <benchmark/benchmark.h>

namespace mongo::stage_builder::abt_lower {
namespace {
void BM_LowerABTLetExpr(benchmark::State& state) {
    auto nLets = state.range(0);
    abt::ABT n = abt::Constant::boolean(true);
    for (int i = 0; i < nLets; i++) {
        n = make<abt::Let>(
            abt::ProjectionName{std::string(str::stream() << "var" << std::to_string(i))},
            abt::Constant::int32(i),
            std::move(n));
    }
    for (auto keepRunning : state) {
        auto env = abt::VariableEnvironment::build(n);
        SlotVarMap map;
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids;
        sbe::InputParamToSlotMap inputParamToSlotMap;
        benchmark::DoNotOptimize(
            SBEExpressionLowering{env, map, runtimeEnv, ids, inputParamToSlotMap}.optimize(n));
        benchmark::ClobberMemory();
    }
}

BENCHMARK(BM_LowerABTLetExpr)->Arg(1)->Arg(10)->Arg(20)->Arg(40)->Arg(100);

void BM_LowerABTMultiLetExpr(benchmark::State& state) {
    auto nLets = state.range(0);
    std::vector<std::pair<abt::ProjectionName, abt::ABT>> vars;
    for (int i = 0; i < nLets; i++) {
        vars.push_back(std::make_pair(
            abt::ProjectionName{std::string(str::stream() << "var" << std::to_string(i))},
            abt::Constant::int32(i)));
    }
    abt::ABT n = abt::make<abt::MultiLet>(std::move(vars), abt::Constant::boolean(true));
    for (auto keepRunning : state) {
        auto env = abt::VariableEnvironment::build(n);
        SlotVarMap map;
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids;
        sbe::InputParamToSlotMap inputParamToSlotMap;
        benchmark::DoNotOptimize(
            SBEExpressionLowering{env, map, runtimeEnv, ids, inputParamToSlotMap}.optimize(n));
        benchmark::ClobberMemory();
    }
}

BENCHMARK(BM_LowerABTMultiLetExpr)->Arg(1)->Arg(10)->Arg(20)->Arg(40)->Arg(100);
}  // namespace
}  // namespace mongo::stage_builder::abt_lower
