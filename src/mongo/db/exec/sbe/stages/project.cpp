// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/project.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/util/str.h"

#include <string_view>


namespace mongo {
namespace sbe {
using namespace std::literals::string_view_literals;
ProjectStage::ProjectStage(std::unique_ptr<PlanStage> input,
                           SlotExprPairVector projects,
                           PlanNodeId nodeId,
                           Binding binding,
                           bool participateInTrialRunTracking)
    : PlanStage(binding == Binding::kSequential ? "project*"sv : "project"sv,
                /*yieldPolicy=*/nullptr,
                nodeId,
                participateInTrialRunTracking),
      _projects(std::move(projects)),
      _binding(binding) {
    _children.emplace_back(std::move(input));

    for (size_t idx = 0; idx < _projects.size(); ++idx) {
        auto [_, inserted] = _slotToIdx.emplace(_projects[idx].first, idx);
        tassert(13572200,
                str::stream() << "project stage defines slot " << _projects[idx].first
                              << " more than once",
                inserted);
    }
}

std::unique_ptr<PlanStage> ProjectStage::clone() const {
    SlotExprPairVector projects;
    for (auto& [k, v] : _projects) {
        projects.emplace_back(k, v->clone());
    }
    return std::make_unique<ProjectStage>(_children[0]->clone(),
                                          std::move(projects),
                                          _commonStats.nodeId,
                                          _binding,
                                          participateInTrialRunTracking());
}

void ProjectStage::prepare(CompileCtx& ctx) {
    _bytecode = ctx.getByteCode();
    _children[0]->prepare(ctx);

    // Compile the expressions in order. With sequential binding, each output slot becomes visible
    // through getAccessor() as soon as its expression is compiled, so later expressions can refer
    // to it. Reserve the full capacity up front so that the accessors of earlier entries never
    // move.
    _compiled.reserve(_projects.size());
    for (auto& [_, expr] : _projects) {
        ctx.root = this;
        auto code = expr->compile(ctx);
        _compiled.push_back({std::move(code), value::OwnedValueAccessor{}});
    }
    _prepared = true;
}

value::SlotAccessor* ProjectStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _slotToIdx.find(slot); it != _slotToIdx.end()) {
        // While the expressions are being compiled, only sequential binding lets an expression see
        // the output slots of the expressions before it.
        if (auto idx = it->second;
            idx < _compiled.size() && (_prepared || _binding == Binding::kSequential)) {
            return &_compiled[idx].accessor;
        }
    }
    return _children[0]->getAccessor(ctx, slot);
}
void ProjectStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
}

PlanState ProjectStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call getNext() on our child so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the getNext() call.
    disableSlotAccess();
    auto state = _children[0]->getNext();

    if (state == PlanState::ADVANCED) {
        // Run the expressions in order so that each one sees the values produced for this row by
        // the expressions before it.
        for (auto& project : _compiled) {
            project.accessor.reset(_bytecode->run(project.code.get()));
        }
    }

    return trackPlanState(state);
}

void ProjectStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> ProjectStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob;
        for (auto&& [slot, expr] : _projects) {
            bob.append(str::stream() << slot, printer.print(expr->debugPrint()));
        }
        ret->debugInfo = BSON("projections" << bob.obj());
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* ProjectStage::getSpecificStats() const {
    return nullptr;
}

void ProjectStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                DebugPrintInfo& debugPrintInfo) const {
    ret.emplace_back("[`");
    bool first = true;
    for (auto&& [slot, expr] : _projects) {
        if (!first) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, slot);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, expr->debugPrint());
        first = false;
    }
    ret.emplace_back("`]");

    DebugPrinter::addNewLine(ret);

    if (debugPrintInfo.printBytecode) {
        int i = 0;
        for (auto& project : _compiled) {
            std::stringstream title;
            title << "FIELD_" << i;
            PlanStage::debugPrintBytecode(ret, project.code, title.str().c_str());
            i++;
        }
    }

    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t ProjectStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_projects);
    size += size_estimator::estimate(_slotToIdx);
    return size;
}

void ProjectStage::doSaveState() {
    for (auto& project : _compiled) {
        prepareForYielding(project.accessor, slotsAccessible());
    }
}

}  // namespace sbe
}  // namespace mongo
