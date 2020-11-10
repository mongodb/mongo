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

#include "mongo/db/exec/sbe/stages/branch.h"

#include "mongo/db/exec/sbe/expressions/expression.h"

namespace mongo {
namespace sbe {
BranchStage::BranchStage(std::unique_ptr<PlanStage> inputThen,
                         std::unique_ptr<PlanStage> inputElse,
                         std::unique_ptr<EExpression> filter,
                         value::SlotVector inputThenVals,
                         value::SlotVector inputElseVals,
                         value::SlotVector outputVals,
                         PlanNodeId planNodeId)
    : PlanStage("branch"_sd, planNodeId),
      _filter(std::move(filter)),
      _inputThenVals(std::move(inputThenVals)),
      _inputElseVals(std::move(inputElseVals)),
      _outputVals(std::move(outputVals)) {
    invariant(_inputThenVals.size() == _outputVals.size());
    invariant(_inputElseVals.size() == _outputVals.size());
    _children.emplace_back(std::move(inputThen));
    _children.emplace_back(std::move(inputElse));
}

std::unique_ptr<PlanStage> BranchStage::clone() const {
    return std::make_unique<BranchStage>(_children[0]->clone(),
                                         _children[1]->clone(),
                                         _filter->clone(),
                                         _inputThenVals,
                                         _inputElseVals,
                                         _outputVals,
                                         _commonStats.nodeId);
}

void BranchStage::prepare(CompileCtx& ctx) {
    value::SlotSet dupCheck;

    _children[0]->prepare(ctx);
    _children[1]->prepare(ctx);

    for (auto slot : _inputThenVals) {
        auto [it, inserted] = dupCheck.insert(slot);
        uassert(4822829, str::stream() << "duplicate field: " << slot, inserted);

        _inputThenAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
    }

    for (auto slot : _inputElseVals) {
        auto [it, inserted] = dupCheck.insert(slot);
        uassert(4822830, str::stream() << "duplicate field: " << slot, inserted);

        _inputElseAccessors.emplace_back(_children[1]->getAccessor(ctx, slot));
    }

    for (auto slot : _outputVals) {
        auto [it, inserted] = dupCheck.insert(slot);
        uassert(4822831, str::stream() << "duplicate field: " << slot, inserted);

        _outValueAccessors.emplace_back(value::ViewOfValueAccessor{});
    }

    // compile filter
    ctx.root = this;
    _filterCode = _filter->compile(ctx);
}

value::SlotAccessor* BranchStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    for (size_t idx = 0; idx < _outputVals.size(); idx++) {
        if (_outputVals[idx] == slot) {
            return &_outValueAccessors[idx];
        }
    }

    return ctx.getAccessor(slot);
}

void BranchStage::open(bool reOpen) {
    _commonStats.opens++;
    _specificStats.numTested++;

    // run the filter expressions here
    auto [owned, tag, val] = _bytecode.run(_filterCode.get());
    if (owned) {
        value::releaseValue(tag, val);
    }
    if (tag == value::TypeTags::Boolean) {
        if (value::bitcastTo<bool>(val)) {
            _activeBranch = 0;
            _children[0]->open(reOpen && _thenOpened);
            _thenOpened = true;
            ++_specificStats.thenBranchOpens;
        } else {
            _activeBranch = 1;
            _children[1]->open(reOpen && _elseOpened);
            _elseOpened = true;
            ++_specificStats.elseBranchOpens;
        }
    } else {
        _activeBranch = boost::none;
    }
}

PlanState BranchStage::getNext() {
    if (!_activeBranch) {
        return trackPlanState(PlanState::IS_EOF);
    }

    if (*_activeBranch == 0) {
        auto state = _children[0]->getNext();
        if (state == PlanState::ADVANCED) {
            for (size_t idx = 0; idx < _outValueAccessors.size(); ++idx) {
                auto [tag, val] = _inputThenAccessors[idx]->getViewOfValue();
                _outValueAccessors[idx].reset(tag, val);
            }
        }
        return trackPlanState(state);
    } else {
        auto state = _children[1]->getNext();
        if (state == PlanState::ADVANCED) {
            for (size_t idx = 0; idx < _outValueAccessors.size(); ++idx) {
                auto [tag, val] = _inputElseAccessors[idx]->getViewOfValue();
                _outValueAccessors[idx].reset(tag, val);
            }
        }
        return trackPlanState(state);
    }
}

void BranchStage::close() {
    _commonStats.closes++;

    if (_thenOpened) {
        _children[0]->close();
        _thenOpened = false;
        ++_specificStats.thenBranchCloses;
    }
    if (_elseOpened) {
        _children[1]->close();
        _elseOpened = false;
        ++_specificStats.elseBranchCloses;
    }
}

std::unique_ptr<PlanStageStats> BranchStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<BranchStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("numTested", _specificStats.numTested);
        bob.appendNumber("thenBranchOpens", _specificStats.thenBranchOpens);
        bob.appendNumber("thenBranchCloses", _specificStats.thenBranchCloses);
        bob.appendNumber("elseBranchOpens", _specificStats.elseBranchOpens);
        bob.appendNumber("elseBranchCloses", _specificStats.elseBranchCloses);
        bob.append("filter", DebugPrinter{}.print(_filter->debugPrint()));
        bob.append("thenSlots", _inputThenVals);
        bob.append("elseSlots", _inputElseVals);
        bob.append("outputSlots", _outputVals);
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    ret->children.emplace_back(_children[1]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* BranchStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> BranchStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();
    ret.emplace_back("{`");
    DebugPrinter::addBlocks(ret, _filter->debugPrint());
    ret.emplace_back("`}");

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outputVals.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _outputVals[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    DebugPrinter::addNewLine(ret);

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _inputThenVals.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _inputThenVals[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    DebugPrinter::addNewLine(ret);

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _inputElseVals.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _inputElseVals[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    DebugPrinter::addBlocks(ret, _children[1]->debugPrint());
    return ret;
}

}  // namespace sbe
}  // namespace mongo
