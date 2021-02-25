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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/loop_join.h"

#include "mongo/util/str.h"

namespace mongo::sbe {
LoopJoinStage::LoopJoinStage(std::unique_ptr<PlanStage> outer,
                             std::unique_ptr<PlanStage> inner,
                             value::SlotVector outerProjects,
                             value::SlotVector outerCorrelated,
                             std::unique_ptr<EExpression> predicate,
                             PlanNodeId nodeId)
    : PlanStage("nlj"_sd, nodeId),
      _outerProjects(std::move(outerProjects)),
      _outerCorrelated(std::move(outerCorrelated)),
      _predicate(std::move(predicate)) {
    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));
}

std::unique_ptr<PlanStage> LoopJoinStage::clone() const {
    return std::make_unique<LoopJoinStage>(_children[0]->clone(),
                                           _children[1]->clone(),
                                           _outerProjects,
                                           _outerCorrelated,
                                           _predicate ? _predicate->clone() : nullptr,
                                           _commonStats.nodeId);
}

void LoopJoinStage::prepare(CompileCtx& ctx) {
    for (auto& f : _outerProjects) {
        auto [it, inserted] = _outerRefs.emplace(f);
        uassert(4822820, str::stream() << "duplicate field: " << f, inserted);
    }
    _children[0]->prepare(ctx);

    for (auto& f : _outerCorrelated) {
        ctx.pushCorrelated(f, _children[0]->getAccessor(ctx, f));
    }
    _children[1]->prepare(ctx);

    for (size_t idx = 0; idx < _outerCorrelated.size(); ++idx) {
        ctx.popCorrelated();
    }

    if (_predicate) {
        ctx.root = this;
        _predicateCode = _predicate->compile(ctx);
    }
}

value::SlotAccessor* LoopJoinStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_outerRefs.count(slot)) {
        return _children[0]->getAccessor(ctx, slot);
    }

    return _children[1]->getAccessor(ctx, slot);
}

void LoopJoinStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
    _outerGetNext = true;
    // Do not open the inner child as we do not have values of correlated parameters yet.
    // The values are available only after we call getNext on the outer side.
}

void LoopJoinStage::openInner() {
    // (re)open the inner side as it can see the correlated value now.
    _children[1]->open(_reOpenInner);
    _reOpenInner = true;
    ++_specificStats.innerOpens;
}

PlanState LoopJoinStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_outerGetNext) {
        auto state = _children[0]->getNext();
        if (state != PlanState::ADVANCED) {
            return trackPlanState(state);
        }

        openInner();
        _outerGetNext = false;
    }

    for (;;) {
        auto state = PlanState::IS_EOF;
        bool pass = false;

        do {
            state = _children[1]->getNext();
            if (state == PlanState::ADVANCED) {
                if (!_predicateCode) {
                    pass = true;
                } else {
                    pass = _bytecode.runPredicate(_predicateCode.get());
                }
            }
        } while (state == PlanState::ADVANCED && !pass);

        if (state == PlanState::ADVANCED) {
            return trackPlanState(PlanState::ADVANCED);
        }
        invariant(state == PlanState::IS_EOF);

        state = _children[0]->getNext();
        if (state != PlanState::ADVANCED) {
            return trackPlanState(state);
        }

        openInner();
    }
}

void LoopJoinStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.closes++;

    if (_reOpenInner) {
        _children[1]->close();

        _reOpenInner = false;
        ++_specificStats.innerCloses;
    }

    _children[0]->close();
}

std::unique_ptr<PlanStageStats> LoopJoinStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("innerOpens", static_cast<long long>(_specificStats.innerOpens));
        bob.appendNumber("innerCloses", static_cast<long long>(_specificStats.innerCloses));
        bob.append("outerProjects", _outerProjects);
        bob.append("outerCorrelated", _outerCorrelated);
        if (_predicate) {
            bob.append("predicate", DebugPrinter{}.print(_predicate->debugPrint()));
        }

        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    ret->children.emplace_back(_children[1]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* LoopJoinStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> LoopJoinStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outerProjects.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _outerProjects[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outerCorrelated.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _outerCorrelated[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    if (_predicate) {
        ret.emplace_back("{`");
        DebugPrinter::addBlocks(ret, _predicate->debugPrint());
        ret.emplace_back("`}");
    }

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    DebugPrinter::addKeyword(ret, "left");
    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);


    DebugPrinter::addKeyword(ret, "right");
    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, _children[1]->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}
}  // namespace mongo::sbe
