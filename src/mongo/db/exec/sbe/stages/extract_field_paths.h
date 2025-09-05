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

#include <memory>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace mongo::sbe {
/**
 * Given an input stage `input` with a single slot `inputSlotId` containing a BSON Object
 * (TODO SERVER-110354 input can be any object type) and a set of requested paths `pathReqs`,
 * populate output slots `outputSlotIds` with the value at each path.
 *
 * This stage is used to evaluate ExpressionFieldPath's in a single walk over the input object.
 *
 * Debug string representation:
 *     extract_field_paths `inputSlotId` pathReqs[outputSlotIds[i] = pathReqs[i], ... ,
 *         outputSlotIds[N] = pathReqs[N]]
 */
class ExtractFieldPathsStage final : public PlanStage {
public:
    ExtractFieldPathsStage(std::unique_ptr<PlanStage> input,
                           value::SlotId inputSlotId,
                           std::vector<value::CellBlock::Path> pathReqs,
                           value::SlotVector outputSlotIds,
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

    const value::SlotId _inputSlotId;
    const std::vector<value::CellBlock::Path> _pathReqs;
    const value::SlotVector _outputSlotIds;

    value::SlotAccessor* _inputAccessor = nullptr;
    std::unique_ptr<value::BsonWalkNode<value::ScalarProjectionPositionInfoRecorder>> _root =
        nullptr;
    std::vector<value::OwnedValueAccessor> _outputAccessors;
    value::SlotMap<size_t> _outputAccessorsIdxForSlotId;
    std::vector<value::ScalarProjectionPositionInfoRecorder> _recorders;
};
}  // namespace mongo::sbe
