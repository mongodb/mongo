// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <vector>

namespace mongo::sbe {
/**
 * The stage iterates all input blocks, which can be a mix of either ValueBlocks or CellBlocks, and
 * populates the corresponding output slots with the individual values from each block. Each input
 * block must be the same size.
 *
 * This stage also takes an optional bitmapSlotId argument. When present, the bitmap slot must
 * contain a block of all booleans, identical in size to the input blocks. Values that lie at an
 * index with a corresponding '0' in the bitmap will be omitted from the output.
 *
 * Debug string representation:
 *
 *  block_to_row blocks[blocks[0], ..., blocks[N]] row[valsOut[0], ..., valsOut[N]] bitset
 */
class BlockToRowStage final : public PlanStage {
public:
    BlockToRowStage(std::unique_ptr<PlanStage> input,
                    value::SlotVector blocks,
                    value::SlotVector valsOut,
                    value::SlotId bitmapSlotId,
                    PlanNodeId nodeId,
                    PlanYieldPolicySBE* yieldPolicy = nullptr,
                    bool participateInTrialRunTracking = true);
    ~BlockToRowStage() override;

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
    void doSaveState() override;


private:
    PlanState getNextFromDeblockedValues();
    void freeDeblockedValueRuns();

    PlanState advanceChild();

    void prepareDeblock();

    const value::SlotVector _blockSlotIds;
    const value::SlotVector _valsOutSlotIds;
    const value::SlotId _bitmapSlotId;

    // Values extracted from the blocks. Initially non-owning views into the block data; after
    // doSaveState() copies them, each element becomes an owning TagValueMaybeOwned.
    std::vector<std::vector<value::TagValueMaybeOwned>> _deblockedValueRuns;

    std::vector<value::SlotAccessor*> _blockAccessors;
    value::SlotAccessor* _bitmapAccessor = nullptr;
    std::vector<value::ViewOfValueAccessor> _valsOutAccessors;

    // Keeps track of the current reading index of the blocks.
    size_t _curIdx = 0;
};
}  // namespace mongo::sbe
