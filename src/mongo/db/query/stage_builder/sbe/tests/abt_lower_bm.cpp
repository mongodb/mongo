/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
