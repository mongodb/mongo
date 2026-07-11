// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

namespace mongo::sbe {
/**
 * Delivers an infinite stream of getNext() calls, always returning 'ADVANCED'. Also, it does not
 * define any slots; i.e. it does not produce any results.
 *
 * On its face value this does not seem to be very useful but it is handy when we have to construct
 * a data stream when there is not any physical source (i.e. no collection to read from).  Typical
 * use cases are: inner side of traverse stage, the outer side of nested loops, constants, etc.
 */
class CoScanStage final : public PlanStage {
public:
    explicit CoScanStage(PlanNodeId,
                         PlanYieldPolicySBE* yieldPolicy = nullptr,
                         bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    size_t estimateCompileTimeSize() const final;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final {}

protected:
};
}  // namespace mongo::sbe
