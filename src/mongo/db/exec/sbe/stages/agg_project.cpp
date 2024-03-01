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

#include "mongo/db/exec/sbe/stages/agg_project.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo {
namespace sbe {
AggProjectStage::AggProjectStage(std::unique_ptr<PlanStage> input,
                                 value::SlotMap<AggExprPair> aggExprPairs,
                                 PlanNodeId nodeId,
                                 bool participateInTrialRunTracking)
    : PlanStage("agg_project"_sd, nullptr /* yieldPolicy */, nodeId, participateInTrialRunTracking),
      _projects(std::move(aggExprPairs)) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> AggProjectStage::clone() const {
    value::SlotMap<AggExprPair> projects;
    for (auto& [k, v] : _projects) {
        projects.emplace(k, AggExprPair{v.init ? v.init->clone() : nullptr, v.acc->clone()});
    }
    return std::make_unique<AggProjectStage>(_children[0]->clone(),
                                             std::move(projects),
                                             _commonStats.nodeId,
                                             _participateInTrialRunTracking);
}

void AggProjectStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Compile agg expressions here.
    for (auto& [slot, aggExprPair] : _projects) {
        auto outAccessor = std::make_unique<value::OwnedValueAccessor>();
        ctx.root = this;
        ctx.aggExpression = true;
        ctx.accumulator = outAccessor.get();
        auto aggCode = aggExprPair.acc->compile(ctx);
        auto initCode = aggExprPair.init ? aggExprPair.init->compile(ctx) : nullptr;
        _slots.emplace_back(slot);
        _initCodes.emplace_back(std::move(initCode));
        _aggCodes.emplace_back(std::move(aggCode));
        _outAccessors.emplace_back(std::move(outAccessor));
        ctx.aggExpression = false;
    }
    _compiled = true;
}

value::SlotAccessor* AggProjectStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled) {
        for (size_t idx = 0; idx < _slots.size(); ++idx) {
            if (_slots[idx] == slot) {
                return _outAccessors[idx].get();
            }
        }
    }
    return _children[0]->getAccessor(ctx, slot);
}
void AggProjectStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    for (size_t idx = 0; idx < _slots.size(); ++idx) {
        if (_initCodes[idx]) {
            auto [owned, tag, val] = _bytecode.run(_initCodes[idx].get());
            _outAccessors[idx]->reset(owned, tag, val);
        } else {
            _outAccessors[idx]->reset();
        }
    }

    _commonStats.opens++;
    _children[0]->open(reOpen);
}

PlanState AggProjectStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    auto state = _children[0]->getNext();

    if (state == PlanState::ADVANCED) {
        // Run the agg project expressions here.
        for (size_t idx = 0; idx < _slots.size(); ++idx) {
            auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].get());
            _outAccessors[idx]->reset(owned, tag, val);
        }
    }

    return trackPlanState(state);
}

void AggProjectStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> AggProjectStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob;
        value::orderedSlotMapTraverse(_projects, [&](auto slot, auto&& expr) {
            auto printBlock = expr.acc->debugPrint();
            if (expr.init) {
                printBlock.emplace_back(DebugPrinter::Block("init{`"));
                DebugPrinter::addBlocks(printBlock, expr.init->debugPrint());
                printBlock.emplace_back(DebugPrinter::Block("`}"));
            }
            bob.append(str::stream() << slot, printer.print(printBlock));
        });
        ret->debugInfo = BSON("projections" << bob.obj());
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* AggProjectStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> AggProjectStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back("[`");
    bool first = true;
    value::orderedSlotMapTraverse(_projects, [&](auto slot, auto&& expr) {
        if (!first) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, slot);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, expr.acc->debugPrint());
        if (expr.init) {
            ret.emplace_back(DebugPrinter::Block("init{`"));
            DebugPrinter::addBlocks(ret, expr.init->debugPrint());
            ret.emplace_back(DebugPrinter::Block("`}"));
        }
        first = false;
    });
    ret.emplace_back("`]");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    return ret;
}

size_t AggProjectStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_projects);
    return size;
}
}  // namespace sbe
}  // namespace mongo
