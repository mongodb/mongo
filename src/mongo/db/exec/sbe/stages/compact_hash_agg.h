// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg_state.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
/**
 * Performs a hash-based grouping. Appears as the "compact_group" stage in debug output. Groups
 * the input based on the provided vector of group-by slots, 'gbs', and computes the accumulators
 * in 'accumulators' for each group.
 *
 * The hash table maps each group-by key (a MaterializedRow, so collation-aware hashing and
 * equality are reused unchanged) to a pointer to one buffer of states per group, laid out by
 * '_layout'.
 * Each accumulator reads its state through its own typed handle and writes its result to its out
 * slot. The table constructs states on insert and destroys them when it is cleared or dropped.
 *
 * Since the data must be buffered in a hash table, this is a "binding reflector". This means
 * slots from the 'input' tree are not visible higher in tree. Stages higher in the tree can
 * only see the slots holding the group-by keys and accumulator results.
 *
 * The group-by keys are visible above the stage through the slots 'gbsOut', parallel to 'gbs'.
 * 'gbsOut[i]' holds the key stored for the group: the first value of 'gbs[i]' seen for the group.
 * It can differ from the value of 'gbs[i]' in other rows of the group, for example under a
 * case-insensitive collation, or for numbers of different types that compare equal.
 *
 * The optional 'collatorSlot', if provided, changes the definition of string equality used when
 * determining whether two group-by keys are equal. For instance, the plan may require us to do a
 * case-insensitive group on a string field.
 *
 * The compact stage does not support spilling: it passes allowDiskUse = false to the base, so if
 * the memory budget is exhausted it fails the query with
 * 'QueryExceededMemoryLimitNoDiskUseAllowed' rather than spilling. This is acceptable only
 * because nothing constructs the stage in production yet; spilling is follow-up work.
 *
 * Debug string representation:
 *
 *  compact_group [<group by slots>] [gbsOut_1 = gbs_1, ..., gbsOut_k = gbs_k]
 *      [slot_1 = accumulator_1, ..., slot_n = accumulator_n]
 *      collatorSlot?
 *      childStage
 */
class CompactHashAggStage final : public HashAggBaseStage<CompactHashAggStage> {
    friend class HashAggBaseStage<CompactHashAggStage>;

public:
    CompactHashAggStage(std::unique_ptr<PlanStage> input,
                        value::SlotVector gbs,
                        value::SlotVector gbsOut,
                        std::vector<std::unique_ptr<CompactHashAggAccumulator>> accumulators,
                        boost::optional<value::SlotId> collatorSlot,
                        PlanYieldPolicySBE* yieldPolicy,
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
    HashAggStats* getHashAggStats();
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const final;

protected:
    // After all inputs are processed (which happens in 'open()'), this method iterates over the
    // resulting groups in memory. Each call updates the '_htIt' iterator, which has the effect
    // of redirecting the '_outKeyAccessors' to reference the group-by keys from their entry in
    // the '_ht' hash table.
    void setIteratorToNextRecord() {
        if (_htIt == _ht->end()) {
            // First invocation of getNext() after open().
            _htIt = _ht->begin();
        } else {
            ++_htIt;
        }
    }

    // Spilling is not supported by the compact stage; the base's memory-limit check fails the
    // query before spilling, so this is never expected to be called.
    void switchToDisk() {
        MONGO_UNREACHABLE_TASSERT(13605508);
    }

private:
    friend class HashAggBaseStage<CompactHashAggStage>;

    using CompactKeyAccessor = value::MaterializedRowKeyAccessor<CompactStateTable::iterator>;

    // --- Hooks used by HashAggBaseStage ---

    boost::optional<CompactStateTable>& ht() {
        return _ht;
    }
    CompactStateTable::iterator& htIt() {
        return _htIt;
    }

    // Spilling is not supported by the compact stage; the base's memory-limit check fails the
    // query before spilling, so this is never expected to be called.
    std::pair<int64_t, int64_t> spillImpl(SpillingStore* recordStore) {
        MONGO_UNREACHABLE_TASSERT(13605507);
    }

    void resetOutputs();

    int64_t estimatedEntrySizeInBytes() const {
        size_t size = _htIt->first.memUsageForSorter() + _layout.size();
        for (const auto& accumulator : _accumulators) {
            size += accumulator->heapMemUsage(_htIt->second);
        }
        return size;
    }

    // ---

    const value::SlotVector _gbs;
    // The slots that expose the group-by keys above this stage, parallel to '_gbs'.
    const value::SlotVector _gbsOut;

    const std::vector<std::unique_ptr<CompactHashAggAccumulator>> _accumulators;

    const CompactStateLayout _layout;

    const boost::optional<value::SlotId> _collatorSlot;

    value::SlotAccessorMap _outAccessors;

    // Accessors used to obtain the values of the group-by slots when reading the input from the
    // child.
    std::vector<value::SlotAccessor*> _inKeyAccessors;

    // Each accessor in '_outKeyAccessors' references one of the keys for a compact hash agg
    // result produced by a 'getNext()' call and reads it from the current entry in the '_ht'
    // hash table according to the '_htIt' iterator.
    std::vector<std::unique_ptr<CompactKeyAccessor>> _outKeyAccessors;

    std::vector<std::unique_ptr<value::OwnedValueAccessor>> _outAggAccessors;

    std::shared_ptr<vm::ByteCode> _bytecode;

    // Hash table where we'll map group-by keys to their per-group state buffers.
    boost::optional<CompactStateTable> _ht;
    CompactStateTable::iterator _htIt;

    // Function object which can be used to check whether two materialized rows of key values are
    // equal. This comparison is collation-aware if the query has a non-simple collation.
    value::MaterializedRowEq _keyEq;

    bool _compiled{false};
    bool _childOpened{false};

    HashAggStats _specificStats;
};  // class CompactHashAggStage
}  // namespace sbe
}  // namespace mongo
