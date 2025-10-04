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

namespace mongo::sbe {
/**
 * This stage is similar to Project stage with the difference being it evaluates accumulator
 * expressions in aggregate context (ie, stateful in contrast to stateless evalution of project)
 *
 * Debug string representation:
 *
 *  agg_project [slot_1 = agg_expr_1 init{init_expr_1}, ..., slot_n = agg_expr_n init{init_expr_n}]
 * childStage
 */
class AggProjectStage final : public PlanStage {
public:
    AggProjectStage(std::unique_ptr<PlanStage> input,
                    AggExprVector aggExprPairs,
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
    bool shouldOptimizeSaveState(size_t) const final {
        return true;
    }

    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
    }

private:
    const AggExprVector _projects;

    // The following four vectors store the output slotId, vm codes for initialisation and
    // aggregation and the output slot accessor for each AggExpr at approprate index so
    // that we retain the mapping among them
    std::vector<value::SlotId> _slots;
    std::vector<std::unique_ptr<vm::CodeFragment>> _initCodes;
    std::vector<std::unique_ptr<vm::CodeFragment>> _aggCodes;
    std::vector<std::unique_ptr<value::OwnedValueAccessor>> _outAccessors;

    vm::ByteCode _bytecode;

    bool _compiled{false};
};
}  // namespace mongo::sbe
