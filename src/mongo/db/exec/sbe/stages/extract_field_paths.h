/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
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
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    void doSaveState() final;
    bool shouldOptimizeSaveState(size_t) const final {
        return true;
    }

    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
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
