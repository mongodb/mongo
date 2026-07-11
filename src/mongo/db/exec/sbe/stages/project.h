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

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {
/**
 * Evaluates a set of expressions and stores the results into corresponding output slots. This is
 * unrelated to projections in MQL. The set of (slot, expression) pairs are passed in the 'projects'
 * slot map.
 *
 * Debug string representation:
 *
 *  project [slot_1 = expr_1, ..., slot_n = expr_n] childStage
 */
class ProjectStage final : public PlanStage {
public:
    ProjectStage(std::unique_ptr<PlanStage> input,
                 SlotExprPairVector projects,
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
    void doSaveState() final;
    bool shouldOptimizeSaveState(size_t) const final {
        return true;
    }


private:
    const SlotExprPairVector _projects;
    value::SlotMap<std::pair<std::unique_ptr<vm::CodeFragment>, value::OwnedValueAccessor>> _fields;

    vm::ByteCode _bytecode;

    bool _compiled{false};
};

template <typename... Ts>
inline auto makeProjectStage(std::unique_ptr<PlanStage> input, PlanNodeId nodeId, Ts&&... pack) {
    return makeS<ProjectStage>(
        std::move(input), makeSlotExprPairVec(std::forward<Ts>(pack)...), nodeId);
}
}  // namespace mongo::sbe
