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

#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/meta/type_traits.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/stage_visitors.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::sbe {
LoopJoinStage::LoopJoinStage(std::unique_ptr<PlanStage> outer,
                             std::unique_ptr<PlanStage> inner,
                             value::SlotVector outerProjects,
                             value::SlotVector outerCorrelated,
                             std::unique_ptr<EExpression> predicate,
                             PlanNodeId nodeId,
                             bool participateInTrialRunTracking)
    : LoopJoinStage(std::move(outer),
                    std::move(inner),
                    std::move(outerProjects),
                    std::move(outerCorrelated),
                    value::SlotVector{},
                    std::move(predicate),
                    JoinType::Inner,
                    nodeId,
                    participateInTrialRunTracking) {}

LoopJoinStage::LoopJoinStage(std::unique_ptr<PlanStage> outer,
                             std::unique_ptr<PlanStage> inner,
                             value::SlotVector outerProjects,
                             value::SlotVector outerCorrelated,
                             value::SlotVector innerProjects,
                             std::unique_ptr<EExpression> predicate,
                             JoinType joinType,
                             PlanNodeId nodeId,
                             bool participateInTrialRunTracking)
    : PlanStage("nlj"_sd, nullptr /* yieldPolicy */, nodeId, participateInTrialRunTracking),
      _outerProjects(std::move(outerProjects)),
      _outerCorrelated(std::move(outerCorrelated)),
      _innerProjects(std::move(innerProjects)),
      _predicate(std::move(predicate)),
      _joinType(joinType) {
    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));

    invariant(_joinType == JoinType::Inner || _joinType == JoinType::Left);
}


std::unique_ptr<PlanStage> LoopJoinStage::clone() const {
    return std::make_unique<LoopJoinStage>(_children[0]->clone(),
                                           _children[1]->clone(),
                                           _outerProjects,
                                           _outerCorrelated,
                                           _innerProjects,
                                           _predicate ? _predicate->clone() : nullptr,
                                           _joinType,
                                           _commonStats.nodeId,
                                           _participateInTrialRunTracking);
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

    if (_joinType == JoinType::Left) {
        for (auto slot : _innerProjects) {
            _outProjectAccessors.emplace(
                slot, value::SwitchAccessor{{_children[1]->getAccessor(ctx, slot), &_constant}});
        }
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
    if (_joinType == JoinType::Left) {
        if (auto it = _outProjectAccessors.find(slot); it != _outProjectAccessors.end()) {
            return &it->second;
        }
        return ctx.getAccessor(slot);
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
    // Reset back to the inputs.
    if (_joinType == JoinType::Left) {
        for (auto&& [k, v] : _outProjectAccessors) {
            v.setIndex(0);
        }
    }
    // (re)open the inner side as it can see the correlated value now.
    _children[1]->open(_reOpenInner);
    _reOpenInner = true;
    ++_specificStats.innerOpens;
}

PlanState LoopJoinStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    bool innerSideMatched = true;

    if (_outerGetNext) {
        auto state = getNextOuterSide();
        if (state != PlanState::ADVANCED) {
            return trackPlanState(state);
        }

        openInner();
        innerSideMatched = false;
        _outerGetNext = false;
    }

    for (;;) {
        while (_children[1]->getNext() == PlanState::ADVANCED) {
            if (!_predicateCode || _bytecode.runPredicate(_predicateCode.get())) {
                return trackPlanState(PlanState::ADVANCED);
            }
        }

        if (_joinType == JoinType::Left && !innerSideMatched) {
            for (auto&& [k, v] : _outProjectAccessors) {
                v.setIndex(1);
            }
            return trackPlanState(PlanState::ADVANCED);
        }

        auto state = getNextOuterSide();
        if (state != PlanState::ADVANCED) {
            return trackPlanState(state);
        }

        openInner();
        innerSideMatched = false;
    }
}

void LoopJoinStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();

    if (_reOpenInner) {
        _children[1]->close();

        _reOpenInner = false;
        ++_specificStats.innerCloses;
    }

    _children[0]->close();
}

void LoopJoinStage::doSaveState(bool relinquishCursor) {
    if (_isReadingLeftSide) {
        // If we yield while reading the left side, there is no need to prepareForYielding() data
        // held in the right side, since we will have to re-open it anyway.
        const bool recursive = true;
        _children[1]->disableSlotAccess(recursive);
    }
}

std::unique_ptr<PlanStageStats> LoopJoinStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    invariant(ret);
    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    ret->children.emplace_back(_children[1]->getStats(includeDebugInfo));
    ret->specific = std::make_unique<LoopJoinStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob(StorageAccessStatsVisitor::collectStats(*this, *ret).toBSON());
        bob.appendNumber("innerOpens", static_cast<long long>(_specificStats.innerOpens))
            .appendNumber("innerCloses", static_cast<long long>(_specificStats.innerCloses))
            .append("outerProjects", _outerProjects.begin(), _outerProjects.end())
            .append("outerCorrelated", _outerCorrelated.begin(), _outerCorrelated.end());
        if (_predicate) {
            bob.append("predicate", DebugPrinter{}.print(_predicate->debugPrint()));
        }

        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* LoopJoinStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> LoopJoinStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    switch (_joinType) {
        case JoinType::Inner:
            ret.emplace_back(DebugPrinter::Block("inner"));
            break;
        case JoinType::Left:
            ret.emplace_back(DebugPrinter::Block("left"));
            break;
        case JoinType::Right:
            ret.emplace_back(DebugPrinter::Block("right"));
            break;
    }

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

size_t LoopJoinStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_outerProjects);
    size += size_estimator::estimate(_outerCorrelated);
    size += _predicate ? _predicate->estimateSize() : 0;
    size += size_estimator::estimate(_specificStats);
    return size;
}
}  // namespace mongo::sbe
