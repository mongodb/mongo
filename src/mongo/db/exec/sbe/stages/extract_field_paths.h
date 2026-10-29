// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/object_walk_node.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>


namespace mongo::sbe {
using PathSlot = std::pair<value::Path, value::SlotId>;
/**
 * 'ExtractFieldPathsStage' takes as input:
 *
 *    - A list of input (path, slotId) pairs. These represent the input slot accessors, and their
 *      location in the path tree. There is either a single (path, slotId) pair that holds the
 *      entire result, or otherwise there is a list of slots for toplevel fields.
 *
 *    - A list of output (path, slotId) pairs. These represent the output slot accessors, and their
 *      location in the path tree. There can be arbitrarily many outputs.
 *
 * The output slot(s) are populated in a single pass over the input slot(s).
 *
 * Example debug string representation:
 *
 *     extract_field_paths inputs[s4 = Get(a)/Id, s5 = Get(b)/Id] outputs[s6 =
 *     Get(a)/Traverse/Get(c)/Id, s7 = Get(b)/Traverse/Get(d)/Id, s8 = Get(b)/Traverse/Get(e)/Id]
 */
class ExtractFieldPathsStage final : public PlanStage {
public:
    ExtractFieldPathsStage(std::unique_ptr<PlanStage> input,
                           std::vector<PathSlot> inputs,
                           std::vector<PathSlot> outputs,
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
    void constructRoot();

    const std::vector<PathSlot> _inputs;
    const std::vector<PathSlot> _outputs;

    std::unique_ptr<value::ObjectWalkNode<value::ScalarProjectionPositionInfoRecorder>> _root =
        nullptr;
    std::vector<value::OwnedValueAccessor> _outputAccessors;
    value::SlotMap<size_t> _outputAccessorsIdxForSlotId;
    std::vector<value::ScalarProjectionPositionInfoRecorder> _recorders;
};
}  // namespace mongo::sbe
