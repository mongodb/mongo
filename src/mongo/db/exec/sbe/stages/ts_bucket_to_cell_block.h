// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/util/modules.h"

namespace mongo::sbe {
/**
 * Given an input stage with a single slot containing a time series bucket BSON document, produces a
 * CellBlock for each path in 'pathReqs' into the output slots 'blocksOut'.
 *
 * Debug string representations:
 *
 *  ts_bucket_to_cellblock bucketSlot pathReqs[blocksOut[0] = paths[0], ...,
 *      blocksOut[N] = paths[N]] metaOut = meta? bitmapSlotId
 *
 * The 'meta' slot contains the bucket's 'meta' field. The 'bitmapSlotId' contains an all 1s
 * bitmap which has 'numMeasurements' entries.
 */
class TsBucketToCellBlockStage final : public PlanStage {
public:
    TsBucketToCellBlockStage(std::unique_ptr<PlanStage> input,
                             value::SlotId bucketSlotId,
                             std::vector<value::PathRequest> pathReqs,
                             value::SlotVector blocksOut,
                             boost::optional<value::SlotId> metaOutSlotId,
                             value::SlotId bitmapOutSlotId,
                             const std::string& timeField,
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
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const final;


protected:
    void doSaveState() final;


private:
    PlanState advanceChild();

    void initCellBlocks();

    const value::SlotId _bucketSlotId;
    const std::vector<value::PathRequest> _pathReqs;
    const value::SlotVector _blocksOutSlotId;
    const boost::optional<value::SlotId> _metaOutSlotId;
    const value::SlotId _bitmapOutSlotId;
    const std::string _timeField;

    value::TsBucketPathExtractor _pathExtractor;

    value::SlotAccessor* _bucketAccessor = nullptr;
    std::vector<value::OwnedValueAccessor> _blocksOutAccessor;
    value::OwnedValueAccessor _metaOutAccessor;
    value::OwnedValueAccessor _bitmapOutAccessor;

    std::vector<std::unique_ptr<value::TsBlock>> _tsBlockStorage;

    TsBucketToBlockStats _specificStats;
};
}  // namespace mongo::sbe
