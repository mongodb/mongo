/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
