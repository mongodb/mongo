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
 * vector.
 *
 * With 'Binding::kParallel' (the default) the bindings have let semantics: no expression may refer
 * to the output slot of any expression in the same stage.
 *
 * With 'Binding::kSequential' the bindings have let* semantics instead: the expressions are
 * evaluated in order, and each expression may refer to the output slots of the expressions that
 * precede it. For example, 'project* [s2 = s1 + 1, s3 = s2 * 2]' computes 's3' from the value of
 * 's2' produced for the same row. An expression still may not refer to its own output slot or to
 * the output slot of an expression that follows it.
 *
 * Debug string representation:
 *
 *  project [slot_1 = expr_1, ..., slot_n = expr_n] childStage
 *  project* [slot_1 = expr_1, ..., slot_n = expr_n] childStage    (Binding::kSequential)
 */
class ProjectStage final : public PlanStage {
public:
    enum class Binding { kParallel, kSequential };

    ProjectStage(std::unique_ptr<PlanStage> input,
                 SlotExprPairVector projects,
                 PlanNodeId planNodeId,
                 Binding binding = Binding::kParallel,
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
    struct CompiledProject {
        std::unique_ptr<vm::CodeFragment> code;
        value::OwnedValueAccessor accessor;
    };

    const SlotExprPairVector _projects;
    const Binding _binding;

    // Maps each output slot to the index of its expression in '_projects'.
    value::SlotMap<size_t> _slotToIdx;

    // The compiled expressions, in the same order as '_projects'. With sequential binding, an
    // output slot becomes visible through getAccessor() as soon as its expression has been
    // compiled, which is what allows each expression to refer to the output slots of the
    // expressions before it. The vector's capacity is reserved up front because compiled code holds
    // pointers to the accessors of earlier entries.
    std::vector<CompiledProject> _compiled;

    // Set once all of the expressions have been compiled. From then on, every output slot is
    // visible through getAccessor() regardless of the binding mode.
    bool _prepared{false};

    vm::ByteCode _bytecode;
};

template <typename... Ts>
inline auto makeProjectStage(std::unique_ptr<PlanStage> input, PlanNodeId nodeId, Ts&&... pack) {
    return makeS<ProjectStage>(
        std::move(input), makeSlotExprPairVec(std::forward<Ts>(pack)...), nodeId);
}

template <typename... Ts>
inline auto makeSequentialProjectStage(std::unique_ptr<PlanStage> input,
                                       PlanNodeId nodeId,
                                       Ts&&... pack) {
    return makeS<ProjectStage>(std::move(input),
                               makeSlotExprPairVec(std::forward<Ts>(pack)...),
                               nodeId,
                               ProjectStage::Binding::kSequential);
}
}  // namespace mongo::sbe
