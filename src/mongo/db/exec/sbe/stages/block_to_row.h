/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"

namespace mongo::sbe {
/**
 * The stage iterates all input blocks, which can be a mix of either ValueBlocks or CellBlocks, and
 * populates the corresponding output slots with the individual values from each block. If the
 * blocks are of unequal size, missing values of blocks that have the less number of elements than
 * the block that maximum number of elements will be filled with Nothing. The number of valsOut must
 * be the same as the number of input blocks.
 *
 * Debug string representation:
 *
 *  block_to_row blocks[blocks[0], ..., blocks[N]] vals[valsOut[0], ..., valsOut[N]]
 */
class BlockToRowStage final : public PlanStage {
public:
    BlockToRowStage(std::unique_ptr<PlanStage> input,
                    value::SlotVector blocks,
                    value::SlotVector valsOut,
                    PlanNodeId nodeId,
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

private:
    PlanState getNextFromCurrentBlocks();

    PlanState advanceChild();

    const value::SlotVector _blockSlotIds;
    const value::SlotVector _valsOutSlotIds;

    std::vector<std::unique_ptr<value::ValueBlock::Cursor>> _blockCursors;
    std::vector<value::SlotAccessor*> _blockAccessors;
    std::vector<value::ViewOfValueAccessor> _valsOutAccessors;
};
}  // namespace mongo::sbe
