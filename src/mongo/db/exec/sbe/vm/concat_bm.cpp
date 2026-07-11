// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/platform/random.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <benchmark/benchmark.h>

namespace mongo::sbe {
namespace {

std::unique_ptr<EExpression> makeConcatExpr(size_t arity, size_t size) {
    static const std::string kAlphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    PseudoRandom random(1 /*seed*/);

    EExpression::Vector args;
    args.reserve(arity);
    for (size_t i = 0; i < arity; ++i) {
        std::string str;
        str.reserve(size);
        for (size_t j = 0; j < size; ++j) {
            str.push_back(kAlphabet[random.nextInt32(kAlphabet.size())]);
        }
        args.push_back(makeE<EConstant>(str));
    }
    return makeE<EFunction>(EFn::kConcat, std::move(args));
}

void runConcatBenchmark(benchmark::State& state, SimpleMemoryUsageTracker* tracker) {
    auto expr = makeConcatExpr(state.range(0) /*arity*/, state.range(1) /*size*/);
    CompileCtx ctx{std::make_unique<RuntimeEnvironment>()};
    vm::CodeFragment code = expr->compileDirect(ctx);

    vm::ByteCode vm;
    vm.setMemoryTracker(tracker);

    for (auto keepRunning : state) {
        auto result = vm.run(&code);
        benchmark::ClobberMemory();
    }
}

void BM_Concat_NoMemoryTracking(benchmark::State& state) {
    runConcatBenchmark(state, nullptr);
}

void BM_Concat_WithMemoryTracking(benchmark::State& state) {
    SimpleMemoryUsageTracker tracker(MemoryUsageLimit{std::numeric_limits<int64_t>::max()});
    runConcatBenchmark(state, &tracker);
}

#define ADD_CONCAT_ARGS()                                                                  \
    Args({2, 10})->Args({2, 100})->Args({10, 10})->Args({10, 100})->Args({100, 10})->Args( \
        {100, 100})

BENCHMARK(BM_Concat_NoMemoryTracking)->ADD_CONCAT_ARGS();
BENCHMARK(BM_Concat_WithMemoryTracking)->ADD_CONCAT_ARGS();

}  // namespace
}  // namespace mongo::sbe
