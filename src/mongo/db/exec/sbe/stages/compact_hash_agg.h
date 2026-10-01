// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
/**
 * Performs a hash-based grouping. Appears as the "compactGroup" stage in debug output. Groups
 * the input based on the provided vector of group-by slots, 'gbs'.
 *
 * This cut of the stage is a skeleton. The hash table maps each group-by key (still a
 * MaterializedRow, so collation-aware hashing and equality are reused unchanged) to one
 * contiguous std::vector<uint8_t> per group, which will hold compact accumulator states once
 * the accumulator contract lands; for now the buffers stay empty and the only outputs are the
 * group-by key slots. Accumulator states, their finalize outputs, and spilling are follow-up
 * work.
 *
 * Since the data must be buffered in a hash table, this is a "binding reflector". This means
 * slots from the 'input' tree are not visible higher in tree. Stages higher in the tree can
 * only see the slots holding the group-by keys.
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
 * The 'optimizedClose' flag controls whether we can close the child subtree right after building
 * the hash table. If true it means that we do not expect the subtree to be reopened.
 *
 * Debug string representation:
 *
 *  compactGroup [<group by slots>] reopen? collatorSlot?
 *      childStage
 */
class CompactHashAggStage final : public HashAggBaseStage<CompactHashAggStage> {
    friend class HashAggBaseStage<CompactHashAggStage>;

public:
    CompactHashAggStage(std::unique_ptr<PlanStage> input,
                        value::SlotVector gbs,
                        bool optimizedClose,
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

    // The compact hash table: group-by keys map to one contiguous byte buffer per group, which
    // will hold compact accumulator states once the accumulator contract lands. The buffers stay
    // empty in this cut.
    using CompactTableType = absl::flat_hash_map<value::MaterializedRow,
                                                 std::vector<uint8_t>,
                                                 value::MaterializedRowHasher,
                                                 value::MaterializedRowEq>;

    using CompactKeyAccessor = value::MaterializedRowKeyAccessor<CompactTableType::iterator>;

    // --- Hooks used by HashAggBaseStage ---

    boost::optional<CompactTableType>& ht() {
        return _ht;
    }
    CompactTableType::iterator& htIt() {
        return _htIt;
    }

    // Spilling is not supported by the compact stage; the base's memory-limit check fails the
    // query before spilling, so this is never expected to be called.
    std::pair<int64_t, int64_t> spillImpl(SpillingStore* recordStore) {
        MONGO_UNREACHABLE_TASSERT(13605507);
    }

    int64_t estimatedEntrySizeInBytes() const {
        return _htIt->first.memUsageForSorter();
    }

    // ---

    const value::SlotVector _gbs;

    const boost::optional<value::SlotId> _collatorSlot;

    // When this operator does not expect to be reopened (almost always) then it can close the
    // child early.
    const bool _optimizedClose{true};

    value::SlotAccessorMap _outAccessors;

    // Accessors used to obtain the values of the group-by slots when reading the input from the
    // child.
    std::vector<value::SlotAccessor*> _inKeyAccessors;

    // Each accessor in '_outKeyAccessors' references one of the keys for a compact hash agg
    // result produced by a 'getNext()' call and reads it from the current entry in the '_ht'
    // hash table according to the '_htIt' iterator.
    std::vector<std::unique_ptr<CompactKeyAccessor>> _outKeyAccessors;

    // Hash table where we'll map group-by keys to their per-group state buffers.
    boost::optional<CompactTableType> _ht;
    CompactTableType::iterator _htIt;

    // Function object which can be used to check whether two materialized rows of key values are
    // equal. This comparison is collation-aware if the query has a non-simple collation.
    value::MaterializedRowEq _keyEq;

    bool _compiled{false};
    bool _childOpened{false};

    HashAggStats _specificStats;
};  // class CompactHashAggStage
}  // namespace sbe
}  // namespace mongo
