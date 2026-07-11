// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace mongo::sbe {
/**
 * VirtualScanStage stores the array 'arr' and getNext() returns each value from the array.
 *
 * This stage mimics the resource management behavior like an actual scan stage. getNext() and
 * doSaveState() release the memory of the returned values. This is useful to expose the potential
 * memory misuse bugs such as heap-use-after-free and memory leaks.
 */
class VirtualScanStage final : public PlanStage {
public:
    explicit VirtualScanStage(PlanNodeId planNodeId,
                              value::SlotId out,
                              value::TagValueMaybeOwned arr,
                              PlanYieldPolicySBE* yieldPolicy = nullptr,
                              bool participateInTrialRunTracking = true);

    ~VirtualScanStage() final = default;

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;

    value::SlotAccessor* getAccessor(sbe::CompileCtx& ctx, sbe::value::SlotId slot) final;

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
    const value::SlotId _outField;

    // Owns the input array if and only if the 'arr' constructor argument owned its value.
    value::TagValueMaybeOwned _arr;

    std::unique_ptr<value::ViewOfValueAccessor> _outFieldOutputAccessor;

    // _index advances on each getNext(); _releaseIndex trails behind to allow the caller to observe
    // the current value before we release the previous one (mimicking scan resource management).
    // Though the TagValueOwned elements in _values would release themselves on destruction anyway,
    // _releaseIndex drives eager release during getNext()/doSaveState() to expose use-after-free
    // bugs — the same way a real scan frees records as it advances past them.
    size_t _index{0};
    size_t _releaseIndex{0};

    std::vector<value::TagValueOwned> _values;
};
}  // namespace mongo::sbe
