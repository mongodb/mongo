// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * Performs a streaming aggregation. Incoming rows with equal group-by keys must be contiguous. For
 * example, they may be sorted on the group-by key or the group-by keys may be unique. Each distinct
 * grouping will produce a single output, consisting of the values of the group-by keys and the
 * results of the aggregate functions.
 *
 * The 'keys' parameter defines the group-by slots. The 'accumulators' parameter is a vector
 * defining an initializer expression, an accumulator expression, and an output slot id.
 *
 * The optional 'collatorSlot', if provided, changes the definition of string equality used when
 * determining whether two adjacent group-by keys are equal. Note this must match the collator of
 * the input.
 *
 * Slots from the input stage are not visible in the output. When a group is finalized, the input
 * slots hold values from the following group (or are EOF).
 *
 * Appears as the "streaming_group" stage in debug output.
 */
class StreamingAggStage final : public PlanStage {
public:
    StreamingAggStage(std::unique_ptr<PlanStage> input,
                      value::SlotVector keys,
                      boost::optional<value::SlotId> collatorSlot,
                      std::vector<std::unique_ptr<HashAggAccumulator>> accumulators,
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
    void readInKey();
    void startGroup();
    void accumulate();
    void endGroup();

    const value::SlotVector _keySlots;
    const boost::optional<value::SlotId> _collatorSlot;

    value::MaterializedRow _curKey;
    value::MaterializedRow _inOutKey;
    value::MaterializedRowEq _keyEq;
    std::vector<value::SlotAccessor*> _inKeyAccessors;
    std::vector<value::MaterializedSingleRowAccessor> _outKeyAccessors;

    std::vector<std::unique_ptr<HashAggAccumulator>> _accumulatorList;
    std::vector<value::OwnedValueAccessor> _curAggAccessors;
    std::vector<value::OwnedValueAccessor> _outAggAccessors;

    value::SlotAccessor* _collatorAccessor = nullptr;
    value::SlotAccessorMap _outAccessors;

    vm::ByteCode _bytecode;

    bool _compiled = false;
    bool _isEOF = false;
};
}  // namespace mongo::sbe
