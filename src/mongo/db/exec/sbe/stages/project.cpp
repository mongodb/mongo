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

#include <set>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/meta/type_traits.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
ProjectStage::ProjectStage(std::unique_ptr<PlanStage> input,
                           SlotExprPairVector projects,
                           PlanNodeId nodeId,
                           bool participateInTrialRunTracking)
    : PlanStage("project"_sd, nullptr /* yieldPolicy */, nodeId, participateInTrialRunTracking),
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
                                          _participateInTrialRunTracking);
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
            auto [owned, tag, val] = _bytecode.run(p.second.first.get());

            // Set the accessors.
            p.second.second.reset(owned, tag, val);
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

std::vector<DebugPrinter::Block> ProjectStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

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
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    return ret;
}

size_t ProjectStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_projects);
    return size;
}

void ProjectStage::doSaveState(bool relinquishCursor) {
    if (!relinquishCursor) {
        return;
    }

    for (auto& [slotId, codeAndAccessor] : _fields) {
        auto& [code, accessor] = codeAndAccessor;
        prepareForYielding(accessor, slotsAccessible());
    }
}

}  // namespace sbe
}  // namespace mongo
