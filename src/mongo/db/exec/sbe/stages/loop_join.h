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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo::sbe {
class LoopJoinStage final : public PlanStage {
public:
    LoopJoinStage(std::unique_ptr<PlanStage> outer,
                  std::unique_ptr<PlanStage> inner,
                  value::SlotVector outerProjects,
                  value::SlotVector outerCorrelated,
                  std::unique_ptr<EExpression> predicate,
                  PlanNodeId nodeId);

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

private:
    PlanState getNextOuterSide() {
        _isReadingLeftSide = true;
        auto ret = _children[0]->getNext();
        _isReadingLeftSide = false;
        return ret;
    }

    // Set of variables coming from the outer side.
    const value::SlotVector _outerProjects;
    // Set of correlated variables from the outer side that are visible on the inner side.
    const value::SlotVector _outerCorrelated;
    // If not set then this is a cross product.
    const std::unique_ptr<EExpression> _predicate;

    value::SlotSet _outerRefs;

    std::vector<value::SlotAccessor*> _correlatedAccessors;
    std::unique_ptr<vm::CodeFragment> _predicateCode;

    vm::ByteCode _bytecode;
    bool _reOpenInner{false};
    bool _outerGetNext{false};
    LoopJoinStats _specificStats;

    // Tracks whether or not we're reading from the left child or the right child.
    // This is necessary for yielding.
    bool _isReadingLeftSide = false;

    void openInner();
};
}  // namespace mongo::sbe
