// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/util/hash_roaring_set.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace mongo::sbe {
/**
 * This stage deduplicates by a given key. Unlike a HashAgg, this stage is not blocking and rows are
 * returned in the same order as they appear in the input stream.
 *
 * TODO: It is possible to optimize this stage in the case where the input is sorted by key X, we
 * are "uniquing" by key Y, and we are guaranteed that all identical values of Y appear are
 * associated with the same key X. In this case the hash table of seen elements can be cleared each
 * time a new key X is encountered.
 *
 * For example, this optimization is possible when the UniqueStage is uniquing by record ID and
 * below it there are non-multikey index scans merged via a SortMerge stage. Each duplicate record
 * ID will be associated with the same sort key.
 *
 * Debug string representation:
 *
 *   unique [<keys>] childStage
 */
class UniqueStage final : public PlanStage {
public:
    UniqueStage(std::unique_ptr<PlanStage> input,
                value::SlotVector keys,
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
    const value::SlotVector _keySlots;

    std::vector<value::SlotAccessor*> _inKeyAccessors;

    // Table of keys that have been seen.
    absl::flat_hash_set<value::MaterializedRow,
                        value::MaterializedRowHasher,
                        value::MaterializedRowEq>
        _seen;
    size_t _prevSeenSizeBytes = 0;
    UniqueStats _specificStats;

    DeduplicatorReporter _dedupReporter;
};

/**
 * This stage is the same as UniqueStage functionally but uses roaring bitmap internally. It can
 * only be used to deduplicate a single integral key.
 *
 * Debug string representation:
 *
 *   unique_roaring [<key>] childStage
 */
class UniqueRoaringStage final : public PlanStage {
public:
    UniqueRoaringStage(std::unique_ptr<PlanStage> input,
                       value::SlotId key,
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
    const value::SlotId _keySlot;
    value::SlotAccessor* _inKeyAccessor = nullptr;
    HashRoaringSet _seen;
    size_t _prevSeenSizeBytes = 0;

    UniqueStats _specificStats;

    DeduplicatorReporter _dedupReporter;
};
}  // namespace mongo::sbe
