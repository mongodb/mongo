// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/hybrid_hash_join.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * Performs a traditional hash join. All rows from the 'inner' side are used to construct a hash
 * table. Keys from the 'outer' side are used to probe the hash table and produce output rows.  This
 * is an equality join where the join is defined by equality between the 'outerCond' slot vector on
 * the outer side and the 'innerCond' slot vector on the inner side. These two slot vectors must
 * have the same length. Each side can define additional slots which appear in each of the rows
 * produced by the join, 'outerProjects' and 'innerProjects'.
 *
 * This is a binding reflector for the inner/build side; since the data is materialized in a hash
 * table, stages higher in the tree cannot see any slots lower in the tree on the inner side. The
 * same applies to the outer/probe side if the hash join spills. However, if the join does not
 * spill, the outer side can stream data as it probes.
 *
 * The optional 'collatorSlot' can be provided to make the join predicate use a special definition
 * for string equality. For example, this can be used to perform a case-insensitive join on string
 * values.
 *
 * Debug string representation:
 *
 *   hj collatorSlot?
 *     left [<outer cond>] [<outer projects>] childStage
 *     right [<inner cond>] [<inner projects>] childStage
 */
class HashJoinStage final : public PlanStage {
public:
    HashJoinStage(std::unique_ptr<PlanStage> outer,
                  std::unique_ptr<PlanStage> inner,
                  value::SlotVector outerCond,
                  value::SlotVector outerProjects,
                  value::SlotVector innerCond,
                  value::SlotVector innerProjects,
                  boost::optional<value::SlotId> collatorSlot,
                  bool allowDiskUse,
                  PlanYieldPolicySBE* yieldPolicy,
                  PlanNodeId planNodeId,
                  boost::optional<size_t> estimatedBuildCardinality,
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
    void doSaveState() final;
    void doRestoreState() final;

private:
    using HashElementAccessor = value::SingleRowPointerAccessor<const value::MaterializedRow*>;

    const value::SlotVector _outerKey;
    const value::SlotVector _outerProjects;
    const value::SlotVector _innerKey;
    const value::SlotVector _innerProjects;
    const boost::optional<value::SlotId> _collatorSlot;
    const bool _allowDiskUse;
    boost::optional<size_t> _estimatedBuildCardinality;

    // All defined values from the inner/outer sides.
    value::SlotAccessorMap _outAccessorMap;

    // Accessors of input condition values (keys) that are being used for probing the hash table.
    std::vector<value::SlotAccessor*> _inOuterKeyAccessors;
    // Accessors of input outer projection values
    std::vector<value::SlotAccessor*> _inOuterProjectAccessors;

    // Accessors of output outer keys.
    std::vector<std::unique_ptr<HashElementAccessor>> _outOuterKeyAccessors;
    // Accessors of output outer projections.
    std::vector<std::unique_ptr<HashElementAccessor>> _outOuterProjectAccessors;

    // Accessors of input condition values (keys) that are being inserted into the hash table.
    std::vector<value::SlotAccessor*> _inInnerKeyAccessors;
    // Accessors of input projection values that are being inserted into the hash table.
    std::vector<value::SlotAccessor*> _inInnerProjectAccessors;

    // Accessors of output keys that are read from the hash table.
    std::vector<std::unique_ptr<HashElementAccessor>> _outInnerKeyAccessors;
    // Accessors of output projections that are read from the hash table.
    std::vector<std::unique_ptr<HashElementAccessor>> _outInnerProjectAccessors;

    // Output rows of keys and projects containing the join result produced by _joinImpl
    const value::MaterializedRow* _outOuterKeyRow = nullptr;
    const value::MaterializedRow* _outOuterProjectRow = nullptr;
    const value::MaterializedRow* _outInnerKeyRow = nullptr;
    const value::MaterializedRow* _outInnerProjectRow = nullptr;

    value::MaterializedRow _probeKey;
    value::MaterializedRow _probeProject;

    // Accessor for collator. Only set if collatorSlot provided during construction.
    value::SlotAccessor* _collatorAccessor = nullptr;

    HashJoinStats _stats;

    boost::optional<HybridHashJoin> _joinImpl;

    /**
     * Cursor Lifecycle in HashJoinStage::getNext():
     *
     * 1. PROBING PHASE (_joinPhase == kProbing):
     *    - For each probe row from inner child, create a cursor via probe()
     *    - Cursor iterates over all matches in hash table
     *    - When cursor exhausted, get next probe row
     *    - When probe child exhausted, move to SPILL_PROCESSING
     *
     * 2. SPILL PROCESSING PHASE (_joinPhase == kSpillProcessing):
     *    - Process spilled partition pairs one at a time
     *    - Each nextSpilledJoinCursor() loads a partition and returns cursor
     *    - Cursor iterates over all matches in that partition
     *    - When cursor exhausted, get next spilled partition
     *    - When no more spilled partitions, move to COMPLETE
     *
     * 3. COMPLETE PHASE (_joinPhase == kComplete):
     *    - Return IS_EOF
     *
     * Note: _cursor is reassigned when transitioning between cursors,
     * and reset when explicitly transitioning between phases.
     */
    enum class JoinPhase {
        kProbing,          // Processing probe side
        kSpillProcessing,  // Processing spilled partitions
        kComplete          // All done
    };

    JoinPhase _joinPhase{JoinPhase::kProbing};
    JoinCursor _cursor = JoinCursor::empty();

    PlanStage* outerChild() const {
        return _children[0].get();
    }
    PlanStage* innerChild() const {
        return _children[1].get();
    }
};
}  // namespace mongo::sbe
