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
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/stage_types.h"

namespace mongo::sbe {
/**
 * Limits the number of results from the child stage, or skips results from the child stage, or
 * both. If both a skip of 's' and a limit of 'l' are provided, first skips 's' results and then
 * limits the remaining results to at most 'l'.
 *
 * Skip and limit values are provided via expressions that are evaluated when the plan is opened.
 *
 * Debug string formats:
 *
 *  limit limitExpression
 *  limitskip limitExpression skipExpression
 *
 * If there is just a skip but no limit, the format is "limitskip none skipExpression".
 */
class LimitSkipStage final : public PlanStage {
public:
    LimitSkipStage(std::unique_ptr<PlanStage> input,
                   std::unique_ptr<EExpression> limit,
                   std::unique_ptr<EExpression> skip,
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

private:
    boost::optional<int64_t> _runLimitOrSkipCode(const vm::CodeFragment* code);

    vm::ByteCode _bytecode;

    std::unique_ptr<EExpression> _limitExpr;
    std::unique_ptr<EExpression> _skipExpr;

    std::unique_ptr<vm::CodeFragment> _limitCode;
    std::unique_ptr<vm::CodeFragment> _skipCode;

    boost::optional<int64_t> _limit;
    boost::optional<int64_t> _skip;
    int64_t _current;
    bool _isEOF;
    LimitSkipStats _specificStats;
};
}  // namespace mongo::sbe
