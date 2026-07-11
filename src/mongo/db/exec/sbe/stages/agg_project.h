// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

namespace mongo::sbe {
/**
 * This stage is similar to Project stage with the difference being it evaluates accumulator
 * expressions in aggregate context (ie, stateful in contrast to stateless evaluation of project)
 *
 * Debug string representation:
 *
 *  agg_project [slot_1 = agg_expr_1 init{init_expr_1}, ..., slot_n = agg_expr_n init{init_expr_n}]
 * childStage
 */
class AggProjectStage final : public PlanStage {
public:
    AggProjectStage(std::unique_ptr<PlanStage> input,
                    AggExprVector aggExprPairs,
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
    const AggExprVector _projects;

    // The following four vectors store the output slotId, vm codes for initialisation and
    // aggregation and the output slot accessor for each AggExpr at approprate index so
    // that we retain the mapping among them
    std::vector<value::SlotId> _slots;
    std::vector<std::unique_ptr<vm::CodeFragment>> _initCodes;
    std::vector<std::unique_ptr<vm::CodeFragment>> _aggCodes;
    std::vector<std::unique_ptr<value::OwnedValueAccessor>> _outAccessors;

    vm::ByteCode _bytecode;

    bool _compiled{false};
};
}  // namespace mongo::sbe
