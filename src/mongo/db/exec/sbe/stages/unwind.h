// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mongo::sbe {
/**
 * Returns the elements of an array and the associated array index one-by-one. The array to unwind
 * is read from the 'inField' slot. The resulting array elements are put into the 'outField' slot
 * with the corresponding array indices available in 'outIndex'.
 *
 * Generally, if 'inField' contains a non-array value it is skipped. However, if
 * 'preserveNullAndEmptyArrays' is true then, as the name implies, null or empty arrays are
 * preserved (i.e. these values are put into the 'outField' slot and the stage returns 'ADVANCED').
 *
 * Debug string representation:
 *
 *   unwind outputValueSlot outputIndexSlot inputSlot preserveNullAndEmptyArrays childStage
 */
class UnwindStage final : public PlanStage {
public:
    UnwindStage(std::unique_ptr<PlanStage> input,
                value::SlotId inField,
                value::SlotId outField,
                value::SlotId outIndex,
                bool preserveNullAndEmptyArrays,
                PlanNodeId planNodeId,
                PlanYieldPolicySBE* yieldPolicy = nullptr,
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
    void doRestoreState() final;


private:
    const value::SlotId _inField;
    const value::SlotId _outField;
    const value::SlotId _outIndex;
    const bool _preserveNullAndEmptyArrays;

    value::SlotAccessor* _inFieldAccessor{nullptr};
    std::unique_ptr<value::OwnedValueAccessor> _outFieldOutputAccessor;
    std::unique_ptr<value::OwnedValueAccessor> _outIndexOutputAccessor;

    value::ArrayAccessor _inArrayAccessor;

    int64_t _index{0};
    bool _inArray{false};
};
}  // namespace mongo::sbe
