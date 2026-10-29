// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace mongo::sbe {
/**
 * This stage performs a merge join given an outer and an inner child stage. The stage remaps
 * both the outer side (buffer to support full cross product) and the inner side to buffer inner
 * values to survive yielding. The join is an equi-join where the join key from the outer side is
 * given by the 'outerKeys' slot vector and the join key from the inner side is given by the
 * 'innerKeys' slot vector. In addition, each resulting row returned by the join has the
 * 'outerProjects' values from the outer side and the 'innerProjects' values from the inner side.
 *
 * The stage expects the data to be sorted according to the 'sortDirs' parameter. This describes the
 * sort direction for each of keys on which we are joining, so the 'sortDirs', 'outerKeys', and
 * 'innerKeys' vectors must each be the same length.
 *
 * Debug string format:
 *
 *   mj [asc|desc, ...]
 *     left [<outer keys>] [<outer projects>] childStage
 *     right [<inner keys>] [<inner projects>] childStage
 */
class MergeJoinStage final : public PlanStage {
public:
    MergeJoinStage(std::unique_ptr<PlanStage> outer,
                   std::unique_ptr<PlanStage> inner,
                   value::SlotVector outerKeys,
                   value::SlotVector outerProjects,
                   value::SlotVector innerKeys,
                   value::SlotVector innerProjects,
                   std::vector<value::SortDirection> sortDirs,
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
    void doSaveState() final;


private:
    using MergeJoinBuffer = std::vector<value::MaterializedRow>;
    using MergeJoinBufferAccessor = value::MaterializedRowAccessor<MergeJoinBuffer>;

    const value::SlotVector _outerKeys;
    const value::SlotVector _outerProjects;
    const value::SlotVector _innerKeys;
    const value::SlotVector _innerProjects;

    const std::vector<value::SortDirection> _dirs;

    // All defined values / projects from the outer side.
    value::SlotAccessorMap _outOuterAccessors;

    // Accessors for outer projects in the buffer.
    std::vector<std::unique_ptr<MergeJoinBufferAccessor>> _outOuterProjectAccessors;

    // Accessors for the inner projects, values are buffered to survive yielding.
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _outInnerProjectAccessors;

    // Accessors for keys which are equivalent for both outer and inner sides.
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _outOuterInnerKeyAccessors;

    // Buffer and its iterator for holding buffered outer projects.
    MergeJoinBuffer _outerProjectsBuffer;
    size_t _outerProjectsBufferIt{0};

    // Key for the outer projects buffer. All rows in the buffer should have this same key.
    value::MaterializedRow _bufferKey;

    std::vector<value::SlotAccessor*> _outerKeyAccessors;
    std::vector<value::SlotAccessor*> _outerProjectAccessors;
    std::vector<value::SlotAccessor*> _innerKeyAccessors;
    std::vector<value::SlotAccessor*> _innerProjectAccessors;

    // The current keys from the outer and inner sides buffered into MaterializedRow's after each
    // side has advanced.
    value::MaterializedRow _currentOuterKey;
    value::MaterializedRow _currentInnerKey;
    // The current project values from inner side buffered into a MaterializedRow after the inner
    // has advanced.
    value::MaterializedRow _currentInnerProject;

    // Comparators for keys from the other and inner sides buffered into MaterializedRow's.
    const value::MaterializedRowEq _rowEq;
    const value::MaterializedRowLess _rowLt;

    // For when need to EOF earlier after exhausting all of the rows from the outer side.
    bool _isOuterDone{false};

    MergeJoinStats _specificStats;
};
}  // namespace mongo::sbe
