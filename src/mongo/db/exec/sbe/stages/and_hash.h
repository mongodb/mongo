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
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * Performs a traditional hash join. All rows from the 'outer' side are used to construct a hash
 * table. Keys from the 'inner' side are used to probe the hash table and produce output rows.  This
 * is an equality join where the join is defined by equality between the 'outerCond' slot vector on
 * the outer side and the 'innerCond' slot vector on the inner side. These two slot vectors must
 * have the same length. Each side can define additional slots which appear in each of the rows
 * produced by the join, 'outerProjects' and 'innerProjects'.
 *
 * This is a binding reflector for the outer/build side; since the data is materialized in a hash
 * table, stages higher in the tree cannot see any slots lower in the tree on the outer side. This
 * is _not_ the case for the inner side, since it can stream data as it probes the hash table.
 *
 * The optional 'collatorSlot' can be provided to make the join predicate use a special definition
 * for string equality. For example, this can be used to perform a case-insensitive join on string
 * values.
 *
 * Debug string representation:
 *
 *   and_hash collatorSlot?
 *     left [<outer cond>] [<outer projects>] childStage
 *     right [<inner cond>] [<inner projects>] childStage
 */
class AndHashStage final : public PlanStage {
public:
    AndHashStage(std::unique_ptr<PlanStage> outer,
                 std::unique_ptr<PlanStage> inner,
                 value::SlotVector outerCond,
                 value::SlotVector outerProjects,
                 value::SlotVector innerCond,
                 value::SlotVector innerProjects,
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
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const final;

protected:
private:
    using TableType = std::unordered_multimap<value::MaterializedRow,  // NOLINT
                                              value::MaterializedRow,
                                              value::MaterializedRowHasher,
                                              value::MaterializedRowEq>;

    using HashKeyAccessor = value::MaterializedRowKeyAccessor<TableType::iterator>;
    using HashProjectAccessor = value::MaterializedRowValueAccessor<TableType::iterator, false>;

    const value::SlotVector _outerCond;
    const value::SlotVector _outerProjects;
    const value::SlotVector _innerCond;
    const value::SlotVector _innerProjects;
    const boost::optional<value::SlotId> _collatorSlot;

    // All defined values from the outer side (i.e. they come from the hash table).
    value::SlotAccessorMap _outOuterAccessors;

    // Accessors of input condition values (keys) that are being inserted into the hash table.
    std::vector<value::SlotAccessor*> _inOuterKeyAccessors;

    // Accessors of output keys.
    std::vector<std::unique_ptr<HashKeyAccessor>> _outOuterKeyAccessors;

    // Accessors of input projection values that are build inserted into the hash table.
    std::vector<value::SlotAccessor*> _inOuterProjectAccessors;

    // Accessors of output projections.
    std::vector<std::unique_ptr<HashProjectAccessor>> _outOuterProjectAccessors;

    // Accessors of input condition values (keys) that are being inserted into the hash table.
    std::vector<value::SlotAccessor*> _inInnerKeyAccessors;

    // Accessor for collator. Only set if collatorSlot provided during construction.
    value::SlotAccessor* _collatorAccessor = nullptr;

    // Key used to probe inside the hash table.
    value::MaterializedRow _probeKey;

    boost::optional<TableType> _ht;
    TableType::iterator _htIt;
    TableType::iterator _htItEnd;

    AndHashStats _specificStats;
};
}  // namespace mongo::sbe
