/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <cstddef>
#include <memory>
#include <utility>
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
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    void doSaveState() final;

    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
    }

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
};
}  // namespace mongo::sbe
