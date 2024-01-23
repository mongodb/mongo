/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <unordered_map>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace sbe {

/**
 * Block equivalent of the HashAgg stage. Only supports algebraic accumulators (median for example
 * is not supported).
 *
 * Debug string representation:
 * block_hashagg [<groupby slot>] [slot_1 = block_expr_1, ..., slot_n = block_expr_n]
 *     [slot_1 = row_expr_1, ..., slot_n = row_expr_n] [_rowAccSlotId]
 * childStage
 */
class BlockHashAggStage final : public PlanStage {
public:
    struct BlockRowAccumulators {
        std::unique_ptr<EExpression> blockAgg;
        std::unique_ptr<EExpression> rowAgg;
    };

    // Map of slot to corresponding accumulators of the form {blockAgg, rowAgg}.
    typedef value::SlotMap<BlockRowAccumulators> BlockAndRowAggs;

    BlockHashAggStage(std::unique_ptr<PlanStage> input,
                      value::SlotId groupSlotId,
                      value::SlotId blockBitsetSlotId,
                      value::SlotId rowAccSlotId,
                      BlockAndRowAggs aggs,
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
    void doSaveState(bool relinquishCursor) override;
    void doRestoreState(bool relinquishCursor) override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;
    void doDetachFromTrialRunTracker() override;
    TrialRunTrackerAttachResultMask doAttachToTrialRunTracker(
        TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) override;

private:
    using TableType = stdx::unordered_map<value::MaterializedRow,
                                          value::MaterializedRow,
                                          value::MaterializedRowHasher,
                                          value::MaterializedRowEq>;

    using HashKeyAccessor = value::MaterializedRowKeyAccessor<TableType::iterator>;
    using HashAggAccessor = value::MaterializedRowValueAccessor<TableType::iterator>;

    // Groupby key slot.
    const value::SlotId _groupSlot;
    // Slot for the bitset corresponding to accumulator input.
    const value::SlotId _blockBitsetSlotId;

    // Used for accumulation after block-level accumulators are run.
    const value::SlotId _rowAccSlotId;
    value::OwnedValueAccessor _internalAccessor;

    /*
     * A map from SlotId to a pair of {blockAccumulator, rowAccumulator}. This SlotId is the
     * input the block accumulator reads from, and is also the output that the row accumulator
     * writes to.
     */
    BlockAndRowAggs _blockRowAggs;

    value::SlotAccessorMap _outAccessorsMap;

    std::vector<value::OwnedValueAccessor> _outAggBlockAccessors;
    std::vector<value::HeterogeneousBlock> _outAggBlocks;

    // Code for block and row accumulators.
    std::vector<std::unique_ptr<vm::CodeFragment>> _blockLevelAggCodes;
    std::vector<std::unique_ptr<vm::CodeFragment>> _aggCodes;

    value::OwnedValueAccessor _outIdBlockAccessor;
    value::HeterogeneousBlock _outIdBlock;

    value::SlotAccessor* _idAccessorIn = nullptr;
    value::SlotAccessor* _blockBitsetAccessorIn = nullptr;

    // Hash table where we'll map groupby key to the accumulators.
    TableType _ht;
    TableType::iterator _htIt;
    std::vector<std::unique_ptr<HashAggAccessor>> _rowAggHtAccessors;
    boost::optional<HashKeyAccessor> _idHtAccessor;

    HashAggStats _specificStats;

    vm::ByteCode _bytecode;
    bool _compiled = false;

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};
};

}  // namespace sbe
}  // namespace mongo
