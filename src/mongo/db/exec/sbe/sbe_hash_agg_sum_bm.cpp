// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/virtual_scan.h"
#include "mongo/db/query/query_test_service_context.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo::sbe {
namespace {

constexpr int32_t kDocs = 500'000;

void run(benchmark::State& state, bool compact) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    value::SlotIdGenerator ids;
    auto [i, a, b, sum, spill, out, keyOut] = std::array{ids.generate(),
                                                         ids.generate(),
                                                         ids.generate(),
                                                         ids.generate(),
                                                         ids.generate(),
                                                         ids.generate(),
                                                         ids.generate()};

    auto docs = value::TagValueOwned::fromRaw(value::makeNewArray());
    for (int32_t doc = 0; doc < kDocs; ++doc) {
        value::getArrayView(docs.value())->push_back(value::TagValueOwned::numberInt32(doc));
    }
    auto cardinality =
        makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(state.range(0)));
    auto input = makeProjectStage(
        makeS<VirtualScanStage>(kEmptyPlanNodeId, i, std::move(docs)),
        kEmptyPlanNodeId,
        a,
        makeE<EFunction>(EFn::kMod, makeEs(makeE<EVariable>(i), std::move(cardinality))),
        b,
        makeE<EVariable>(i));

    std::unique_ptr<PlanStage> root;
    if (compact) {
        std::vector<std::unique_ptr<CompactHashAggAccumulator>> accumulators;
        accumulators.push_back(
            std::make_unique<CompactSumHashAggAccumulatorTerminal>(out, makeE<EVariable>(b)));
        root = makeS<CompactHashAggStage>(std::move(input),
                                          makeSV(a),
                                          std::move(accumulators),
                                          boost::none,
                                          nullptr,
                                          kEmptyPlanNodeId);
    } else {
        std::vector<std::unique_ptr<HashAggAccumulator>> accumulators;
        accumulators.push_back(std::make_unique<CompiledHashAggAccumulator>(
            sum,
            spill,
            makeE<EFunction>(EFn::kAggDoubleDoubleSum, makeEs(makeE<EVariable>(b))),
            makeE<EFunction>(EFn::kAggMergeDoubleDoubleSums, makeEs(makeE<EVariable>(spill)))));
        root = makeProjectStage(
            makeS<HashAggStage>(std::move(input),
                                makeSV(a),
                                makeSV(keyOut),
                                std::move(accumulators),
                                true,
                                boost::none,
                                false,
                                nullptr,
                                kEmptyPlanNodeId),
            kEmptyPlanNodeId,
            out,
            makeE<EFunction>(EFn::kDoubleDoubleSumFinalize, makeEs(makeE<EVariable>(sum))));
    }

    CompileCtx ctx(std::make_unique<RuntimeEnvironment>());
    root->attachToOperationContext(opCtx.get());
    root->prepare(ctx);
    auto accessor = root->getAccessor(ctx, out);
    root->open(false);
    for (auto _ : state) {
        root->open(true);
        while (root->getNext() == PlanState::ADVANCED) {
            benchmark::DoNotOptimize(accessor->getViewOfValue());
        }
    }
    root->close();
    state.SetItemsProcessed(state.iterations() * kDocs);
}

void compiled(benchmark::State& state) {
    run(state, false);
}

void compact(benchmark::State& state) {
    run(state, true);
}

BENCHMARK(compiled)->RangeMultiplier(2)->Range(2, 1 << 18)->Arg(kDocs);
BENCHMARK(compact)->RangeMultiplier(2)->Range(2, 1 << 18)->Arg(kDocs);

}  // namespace
}  // namespace mongo::sbe
