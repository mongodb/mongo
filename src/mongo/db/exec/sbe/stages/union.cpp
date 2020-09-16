/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/union.h"

#include "mongo/db/exec/sbe/expressions/expression.h"

namespace mongo::sbe {
UnionStage::UnionStage(std::vector<std::unique_ptr<PlanStage>> inputStages,
                       std::vector<value::SlotVector> inputVals,
                       value::SlotVector outputVals,
                       PlanNodeId planNodeId)
    : PlanStage("union"_sd, planNodeId),
      _inputVals{std::move(inputVals)},
      _outputVals{std::move(outputVals)} {
    _children = std::move(inputStages);

    invariant(_children.size() > 0);
    invariant(_children.size() == _inputVals.size());
    invariant(std::all_of(
        _inputVals.begin(), _inputVals.end(), [size = _outputVals.size()](const auto& slots) {
            return slots.size() == size;
        }));
}

std::unique_ptr<PlanStage> UnionStage::clone() const {
    std::vector<std::unique_ptr<PlanStage>> inputStages;
    for (auto& child : _children) {
        inputStages.emplace_back(child->clone());
    }
    return std::make_unique<UnionStage>(
        std::move(inputStages), _inputVals, _outputVals, _commonStats.nodeId);
}

void UnionStage::prepare(CompileCtx& ctx) {
    value::SlotSet dupCheck;

    for (size_t childNum = 0; childNum < _children.size(); childNum++) {
        _children[childNum]->prepare(ctx);

        for (auto slot : _inputVals[childNum]) {
            auto [it, inserted] = dupCheck.insert(slot);
            uassert(4822806, str::stream() << "duplicate field: " << slot, inserted);

            _inValueAccessors[_children[childNum].get()].emplace_back(
                _children[childNum]->getAccessor(ctx, slot));
        }
    }

    for (auto slot : _outputVals) {
        auto [it, inserted] = dupCheck.insert(slot);
        uassert(4822807, str::stream() << "duplicate field: " << slot, inserted);

        _outValueAccessors.emplace_back(value::ViewOfValueAccessor{});
    }
}

value::SlotAccessor* UnionStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    for (size_t idx = 0; idx < _outputVals.size(); idx++) {
        if (_outputVals[idx] == slot) {
            return &_outValueAccessors[idx];
        }
    }

    return ctx.getAccessor(slot);
}

void UnionStage::open(bool reOpen) {
    _commonStats.opens++;
    if (reOpen) {
        std::queue<UnionBranch> emptyQueue;
        swap(_remainingBranchesToDrain, emptyQueue);
    }

    for (auto& child : _children) {
        _remainingBranchesToDrain.push({child.get(), reOpen});
    }

    _remainingBranchesToDrain.front().open();
    _currentStage = _remainingBranchesToDrain.front().stage;
}

PlanState UnionStage::getNext() {
    auto state = PlanState::IS_EOF;

    while (!_remainingBranchesToDrain.empty() && state != PlanState::ADVANCED) {
        if (!_currentStage) {
            auto& branch = _remainingBranchesToDrain.front();
            branch.open();
            _currentStage = branch.stage;
        }
        state = _currentStage->getNext();

        if (state == PlanState::IS_EOF) {
            _currentStage = nullptr;
            _remainingBranchesToDrain.front().close();
            _remainingBranchesToDrain.pop();
        } else {
            const auto& inValueAccessors = _inValueAccessors[_currentStage];

            for (size_t idx = 0; idx < inValueAccessors.size(); idx++) {
                auto [tag, val] = inValueAccessors[idx]->getViewOfValue();
                _outValueAccessors[idx].reset(tag, val);
            }
        }
    }

    return trackPlanState(state);
}

void UnionStage::close() {
    _commonStats.closes++;
    _currentStage = nullptr;
    while (!_remainingBranchesToDrain.empty()) {
        _remainingBranchesToDrain.front().close();
        _remainingBranchesToDrain.pop();
    }
}

std::unique_ptr<PlanStageStats> UnionStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    for (auto&& child : _children) {
        ret->children.emplace_back(child->getStats());
    }
    return ret;
}

const SpecificStats* UnionStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> UnionStage::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "union");

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outputVals.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _outputVals[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    for (size_t childNum = 0; childNum < _children.size(); childNum++) {
        ret.emplace_back(DebugPrinter::Block("[`"));
        for (size_t idx = 0; idx < _inputVals[childNum].size(); idx++) {
            if (idx) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }
            DebugPrinter::addIdentifier(ret, _inputVals[childNum][idx]);
        }
        ret.emplace_back(DebugPrinter::Block("`]"));

        DebugPrinter::addBlocks(ret, _children[childNum]->debugPrint());

        if (childNum + 1 < _children.size()) {
            DebugPrinter::addNewLine(ret);
        }
    }
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}
}  // namespace mongo::sbe
