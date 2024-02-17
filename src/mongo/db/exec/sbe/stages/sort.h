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

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/sorter/sorter_stats.h"

namespace mongo {
template <typename Key, typename Value>
class SortIteratorInterface;

template <typename Key, typename Value>
class Sorter;
}  // namespace mongo

namespace mongo::sbe {
/**
 * Sorts the incoming data from the 'input' tree. The keys on which we are sorting are given by the
 * order-by slots, 'obs'.  The ascending/descending sort direction associated with each of these
 * order-by slots is given by 'dirs'. The 'obs' and 'dirs' vectors must be the same length. The
 * 'vals' slot vector indicates the values that should associated with the sort keys.
 *
 * Together, a set of values for 'obs' and 'vals' consistute one of the rows being sorted. These
 * rows are materialized at runtime. The given 'memoryLimit' contrains the amount of materialized
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
              PlanYieldPolicy* yieldPolicy,
              PlanNodeId planNodeId,
              bool participateInTrialRunTracking = true);

    ~SortStage();

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
    void doDetachFromTrialRunTracker() override;
    TrialRunTrackerAttachResultMask doAttachToTrialRunTracker(
        TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) override;

private:
    class SortIface {
    public:
        virtual ~SortIface() {}
        virtual void prepare(CompileCtx& ctx) = 0;
        virtual value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) = 0;
        virtual void open(bool reOpen) = 0;
        virtual PlanState getNext() = 0;
        virtual void close() = 0;
    };

    template <typename KeyRow, typename ValueRow>
    class SortImpl : public SortIface {
    public:
        SortImpl(SortStage& stage);
        ~SortImpl();

        void prepare(CompileCtx& ctx) final;
        value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
        void open(bool reOpen) final;
        PlanState getNext() final;
        void close() final;

    private:
        int64_t runLimitCode();
        void makeSorter();

        using SorterIterator = SortIteratorInterface<KeyRow, ValueRow>;
        using SorterData = std::pair<KeyRow, ValueRow>;

        SortStage& _stage;

        std::vector<value::SlotAccessor*> _inKeyAccessors;
        std::vector<value::SlotAccessor*> _inValueAccessors;

        value::SlotMap<std::unique_ptr<value::SlotAccessor>> _outAccessors;

        std::unique_ptr<SorterIterator> _mergeIt;
        SorterData _mergeData;
        SorterData* _mergeDataIt{&_mergeData};
        std::unique_ptr<Sorter<KeyRow, ValueRow>> _sorter;

        std::unique_ptr<vm::CodeFragment> _limitCode;
    };

private:
    template <typename KeyType, typename ValueType>
    std::unique_ptr<SortIface> makeStageImplInternal();
    template <typename KeyType>
    std::unique_ptr<SortIface> makeStageImplInternal(size_t valueSize);
    std::unique_ptr<SortIface> makeStageImplInternal(size_t keySize, size_t valueSize);

    std::unique_ptr<SortIface> makeStageImpl();

private:
    const value::SlotVector _obs;
    const std::vector<value::SortDirection> _dirs;
    const value::SlotVector _vals;
    const bool _allowDiskUse;

    std::unique_ptr<SorterFileStats> _sorterFileStats;

    std::unique_ptr<SortIface> _stageImpl;

    std::unique_ptr<EExpression> _limitExpr;

    SortStats _specificStats;
    // If provided, used during a trial run to accumulate certain execution stats. Once the
    // trial run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};
};
}  // namespace mongo::sbe
