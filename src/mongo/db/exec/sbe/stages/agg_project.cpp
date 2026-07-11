// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/agg_project.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"

#include <string_view>

namespace mongo {
namespace sbe {
using namespace std::literals::string_view_literals;
AggProjectStage::AggProjectStage(std::unique_ptr<PlanStage> input,
                                 AggExprVector aggExprPairs,
                                 PlanNodeId nodeId,
                                 bool participateInTrialRunTracking)
    : PlanStage("agg_project"sv, nullptr /* yieldPolicy */, nodeId, participateInTrialRunTracking),
      _projects(std::move(aggExprPairs)) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> AggProjectStage::clone() const {
    AggExprVector projects;
    for (auto& [k, v] : _projects) {
        projects.emplace_back(k, AggExprPair{v.init ? v.init->clone() : nullptr, v.agg->clone()});
    }
    return std::make_unique<AggProjectStage>(_children[0]->clone(),
                                             std::move(projects),
                                             _commonStats.nodeId,
                                             participateInTrialRunTracking());
}

void AggProjectStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Compile agg expressions here.
    for (auto& [slot, aggExprPair] : _projects) {
        auto outAccessor = std::make_unique<value::OwnedValueAccessor>();
        ctx.root = this;
        ctx.aggExpression = true;
        ctx.accumulator = outAccessor.get();
        auto aggCode = aggExprPair.agg->compile(ctx);
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
            _outAccessors[idx]->reset(_bytecode.run(_initCodes[idx].get()));
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
            _outAccessors[idx]->reset(_bytecode.run(_aggCodes[idx].get()));
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
        for (const auto& [slot, expr] : _projects) {
            auto printBlock = expr.agg->debugPrint();
            if (expr.init) {
                printBlock.emplace_back(DebugPrinter::Block("init{`"));
                DebugPrinter::addBlocks(printBlock, expr.init->debugPrint());
                printBlock.emplace_back(DebugPrinter::Block("`}"));
            }
            bob.append(str::stream() << slot, printer.print(printBlock));
        }
        ret->debugInfo = BSON("projections" << bob.obj());
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* AggProjectStage::getSpecificStats() const {
    return nullptr;
}

void AggProjectStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                   DebugPrintInfo& debugPrintInfo) const {

    ret.emplace_back("[`");
    bool first = true;
    for (const auto& [slot, expr] : _projects) {
        if (!first) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, slot);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, expr.agg->debugPrint());
        if (expr.init) {
            ret.emplace_back(DebugPrinter::Block("init{`"));
            DebugPrinter::addBlocks(ret, expr.init->debugPrint());
            ret.emplace_back(DebugPrinter::Block("`}"));
        }
        first = false;
    }
    ret.emplace_back("`]");

    DebugPrinter::addNewLine(ret);

    if (debugPrintInfo.printBytecode) {
        int i = 0;
        for (const std::unique_ptr<vm::CodeFragment>& code : _initCodes) {
            std::stringstream title;
            title << "INIT_" << i;
            PlanStage::debugPrintBytecode(ret, code, title.str().c_str());
            i++;
        }
        i = 0;
        for (const std::unique_ptr<vm::CodeFragment>& code : _aggCodes) {
            std::stringstream title;
            title << "AGG_" << i;
            PlanStage::debugPrintBytecode(ret, code, title.str().c_str());
            i++;
        }
    }

    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t AggProjectStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_projects);
    return size;
}
}  // namespace sbe
}  // namespace mongo
