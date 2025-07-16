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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mongo::sbe {
enum class JoinType : uint8_t { Inner, Left, Right };

/**
 * Implements a traditional nested loop join. For each advance from the 'outer' child, re-opens the
 * 'inner' child and calls 'getNext()' on the inner child until EOF. The caller can optionally
 * provide a join 'predicate' which is evaluated once per pair of outer and inner rows. If no
 * predicate expression is provided, then the Cartesian product is produced.
 *
 * For symmetry with hash join, this is a binding reflector on the outer side. Nodes higher in the
 * tree can only access those slots from the outer side that are named in 'outerProjects'. All slots
 * from the inner side are visible.
 *
 * The 'outerCorrelated' slots are slots from the outer side which are made visible to the inner
 * side.
 *
 * Debug string format:
 *
 *  nlj [<outer projects>] [<outer correlated>] { predicate }
 *      left childStage
 *      right childStage
 */
class LoopJoinStage final : public PlanStage {
public:
    // Legacy constructor.
    LoopJoinStage(std::unique_ptr<PlanStage> outer,
                  std::unique_ptr<PlanStage> inner,
                  value::SlotVector outerProjects,
                  value::SlotVector outerCorrelated,
                  std::unique_ptr<EExpression> predicate,
                  PlanNodeId planNodeId,
                  bool participateInTrialRunTracking = true);

    LoopJoinStage(std::unique_ptr<PlanStage> outer,
                  std::unique_ptr<PlanStage> inner,
                  value::SlotVector outerProjects,
                  value::SlotVector outerCorrelated,
                  value::SlotVector innerProjects,
                  std::unique_ptr<EExpression> predicate,
                  JoinType joinType,
                  PlanNodeId planNodeId,
                  bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;
    void doSaveState() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    bool shouldOptimizeSaveState(size_t idx) const final {
        // LoopJoinStage::getNext() only guarantees that the inner child's getNext() was called.
        // Thus, it is safe to propagate disableSlotAccess to the inner child, but not to the outer
        // child.
        return idx == 1;
    }

    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
    }

private:
    PlanState getNextOuterSide() {
        _isReadingLeftSide = true;
        auto ret = _children[0]->getNext();
        _isReadingLeftSide = false;
        return ret;
    }

    void openInner();

    // Set of variables coming from the outer side. These are _not_ visible to the inner side,
    // unless also added to '_outerCorrelated'.
    const value::SlotVector _outerProjects;

    // Set of correlated variables from the outer side that are visible on the inner side.
    const value::SlotVector _outerCorrelated;

    const value::SlotVector _innerProjects;

    // Predicate to filter the joint set. If not set then the result is a cross product.
    // Note: the predicate resolves the slots it's using through this stage's public accessors,
    // meaning that if they are coming from the 'outer', they must be projected by the 'outer'.
    const std::unique_ptr<EExpression> _predicate;

    vm::ByteCode _bytecode;
    std::unique_ptr<vm::CodeFragment> _predicateCode;

    // Switching between the input and Nothing/null for outer joins. Unused for inner joins.
    value::SlotMap<value::SwitchAccessor> _outProjectAccessors;
    // Defaults to Nothing. We have to explicitely reset to null if we want the null extenstion.
    value::OwnedValueAccessor _constant;

    // '_outerProjects' as a set (for faster checking of accessors, provided by the 'outer' child).
    value::SlotSet _outerRefs;

    LoopJoinStats _specificStats;

    const JoinType _joinType;

    bool _reOpenInner{false};
    bool _outerGetNext{false};

    // Tracks whether or not we're reading from the left child or the right child.
    // This is necessary for yielding.
    bool _isReadingLeftSide = false;
};
}  // namespace mongo::sbe
