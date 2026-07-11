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
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * This stage delivers results from either 'then' or 'else' branch depending on the value of the
 * 'filter' expression as evaluated during the open() call. Debug string representation:
 *
 *   branch { expr } [<output slots>] [<then slots>] thenChildStage [<else slots>] elseChildStage
 *
 * When this stage returns 'PlanStage::ADVANCED', the vector of output slots will hold values from
 * either the <then slots> slot vector or the <else slots> slot vector, depending on which branch is
 * engaged.
 */
class BranchStage final : public PlanStage {
public:
    BranchStage(std::unique_ptr<PlanStage> inputThen,
                std::unique_ptr<PlanStage> inputElse,
                std::unique_ptr<EExpression> filter,
                value::SlotVector inputThenVals,
                value::SlotVector inputElseVals,
                value::SlotVector outputVals,
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
    bool shouldOptimizeSaveState(size_t idx) const final {
        return _activeBranch && (static_cast<size_t>(*_activeBranch) == idx);
    }


private:
    const std::unique_ptr<EExpression> _filter;
    const value::SlotVector _inputThenVals;
    const value::SlotVector _inputElseVals;
    const value::SlotVector _outputVals;
    std::unique_ptr<vm::CodeFragment> _filterCode;

    std::vector<value::SwitchAccessor> _outValueAccessors;

    boost::optional<int> _activeBranch;
    bool _thenOpened{false};
    bool _elseOpened{false};

    vm::ByteCode _bytecode;
    BranchStats _specificStats;
};
}  // namespace mongo::sbe
