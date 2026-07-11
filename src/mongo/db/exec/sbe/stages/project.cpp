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
                           bool participateInTrialRunTracking)
    : PlanStage("project"sv, nullptr /* yieldPolicy */, nodeId, participateInTrialRunTracking),
      _projects(std::move(projects)) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> ProjectStage::clone() const {
    SlotExprPairVector projects;
    for (auto& [k, v] : _projects) {
        projects.emplace_back(k, v->clone());
    }
    return std::make_unique<ProjectStage>(_children[0]->clone(),
                                          std::move(projects),
                                          _commonStats.nodeId,
                                          participateInTrialRunTracking());
}

void ProjectStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Compile project expressions here.
    for (auto& [slot, expr] : _projects) {
        ctx.root = this;
        auto code = expr->compile(ctx);
        _fields[slot] = {std::move(code), value::OwnedValueAccessor{}};
    }
    _compiled = true;
}

value::SlotAccessor* ProjectStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _fields.find(slot); _compiled && it != _fields.end()) {
        return &it->second.second;
    } else {
        return _children[0]->getAccessor(ctx, slot);
    }
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
        // Run the project expressions here.
        for (auto& p : _fields) {
            // Set the accessors.
            p.second.second.reset(_bytecode.run(p.second.first.get()));
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
        for (auto& p : _fields) {
            std::stringstream title;
            title << "FIELD_" << i;
            PlanStage::debugPrintBytecode(ret, p.second.first, title.str().c_str());
            i++;
        }
    }

    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t ProjectStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_projects);
    return size;
}

void ProjectStage::doSaveState() {
    for (auto& [slotId, codeAndAccessor] : _fields) {
        auto& [code, accessor] = codeAndAccessor;
        prepareForYielding(accessor, slotsAccessible());
    }
}

}  // namespace sbe
}  // namespace mongo
