// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * Limits the number of results from the child stage, or skips results from the child stage, or
 * both. If both a skip of 's' and a limit of 'l' are provided, first skips 's' results and then
 * limits the remaining results to at most 'l'.
 *
 * Skip and limit values are provided via expressions that are evaluated when the plan is opened.
 *
 * Debug string formats:
 *
 *  limit limitExpression
 *  limitskip limitExpression skipExpression
 *
 * If there is just a skip but no limit, the format is "limitskip none skipExpression".
 */
class LimitSkipStage final : public PlanStage {
public:
    LimitSkipStage(std::unique_ptr<PlanStage> input,
                   std::unique_ptr<EExpression> limit,
                   std::unique_ptr<EExpression> skip,
                   PlanNodeId planNodeId,
                   bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const final;

protected:
    bool shouldOptimizeSaveState(size_t) const final {
        return true;
    }


private:
    boost::optional<int64_t> _runLimitOrSkipCode(const vm::CodeFragment* code);

    vm::ByteCode _bytecode;

    std::unique_ptr<EExpression> _limitExpr;
    std::unique_ptr<EExpression> _skipExpr;

    std::unique_ptr<vm::CodeFragment> _limitCode;
    std::unique_ptr<vm::CodeFragment> _skipCode;

    boost::optional<int64_t> _limit;
    boost::optional<int64_t> _skip;
    int64_t _current;
    LimitSkipStats _specificStats;
};
}  // namespace mongo::sbe
