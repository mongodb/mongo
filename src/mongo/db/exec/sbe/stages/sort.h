// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace mongo::sbe {
/**
 * Sorts the incoming data from the 'input' tree. The keys on which we are sorting are given by the
 * order-by slots, 'obs'.  The ascending/descending sort direction associated with each of these
 * order-by slots is given by 'dirs'. The 'obs' and 'dirs' vectors must be the same length. The
 * 'vals' slot vector indicates the values that should associated with the sort keys.
 *
 * Together, a set of values for 'obs' and 'vals' constitute one of the rows being sorted. These
 * rows are materialized at runtime. The given 'memoryLimit' contains the amount of materialized
 * data that can be held in memory. If this limit is exceeded, and 'allowDiskUse' is false, then
 * this stage throws a query-fatal exception. If 'allowDiskUse' is true, then this stage will spill
 * materialized rows to disk.
 *
 * If 'limit' is not std::numeric_limits<size_t>::max(), then this is a top-k sort that should only
 * return the number of rows given by the limit.
 *
 * This stage is a binding reflector, meaning that only the 'obs' and 'vals' slots are visible to
 * nodes higher in the tree.
 *
 * Debug string representation:
 *
 *   sort [<order-by slots>] [asc/desc, ...] [<value slots>] limit? childStage
 */
class SortStage final : public PlanStage {
public:
    SortStage(std::unique_ptr<PlanStage> input,
              value::SlotVector obs,
              std::vector<value::SortDirection> dirs,
              value::SlotVector vals,
              std::unique_ptr<EExpression> limit,
              size_t memoryLimit,
              bool allowDiskUse,
              PlanYieldPolicySBE* yieldPolicy,
              PlanNodeId planNodeId,
              bool participateInTrialRunTracking = true);

    ~SortStage() override;

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
private:
    void doForceSpill() override {
        if (_stageImpl) {
            _stageImpl->forceSpill();
        }
    }

    class SortIface {
    public:
        virtual ~SortIface() = default;
        virtual void prepare(CompileCtx& ctx) = 0;
        virtual value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) = 0;
        virtual void open(bool reOpen) = 0;
        virtual PlanState getNext() = 0;
        virtual void close() = 0;
        virtual void forceSpill() = 0;
    };

    /** Implements `SortIface`. Defined in `sort_stage_sort_impl.cpp`. */
    template <typename KeyRow, typename ValueRow>
    class SortImpl;

    /** Defined in `sort_stage_sort_impl.cpp`. */
    std::unique_ptr<SortIface> _makeStageImpl();

    const value::SlotVector _obs;
    const std::vector<value::SortDirection> _dirs;
    const value::SlotVector _vals;
    const bool _allowDiskUse;

    std::unique_ptr<SortIface> _stageImpl;

    std::unique_ptr<EExpression> _limitExpr;

    SortStats _specificStats;
};
}  // namespace mongo::sbe
