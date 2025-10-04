/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy.h"

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
                PlanYieldPolicy* yieldPolicy = nullptr,
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
    void doRestoreState() final;

    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
    }

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
