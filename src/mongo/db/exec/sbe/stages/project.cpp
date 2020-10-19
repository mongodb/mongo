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

#include "mongo/db/exec/sbe/stages/project.h"

namespace mongo {
namespace sbe {
ProjectStage::ProjectStage(std::unique_ptr<PlanStage> input,
                           value::SlotMap<std::unique_ptr<EExpression>> projects,
                           PlanNodeId nodeId)
    : PlanStage("project"_sd, nodeId), _projects(std::move(projects)) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> ProjectStage::clone() const {
    value::SlotMap<std::unique_ptr<EExpression>> projects;
    for (auto& [k, v] : _projects) {
        projects.emplace(k, v->clone());
    }
    return std::make_unique<ProjectStage>(
        _children[0]->clone(), std::move(projects), _commonStats.nodeId);
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
    _commonStats.opens++;
    _children[0]->open(reOpen);
}

PlanState ProjectStage::getNext() {
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
    _commonStats.closes++;
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> ProjectStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats());
    return ret;
}

const SpecificStats* ProjectStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> ProjectStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();
    ret.emplace_back("[`");
    bool first = true;
    for (auto& p : _projects) {
        if (!first) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, p.first);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, p.second->debugPrint());
        first = false;
    }
    ret.emplace_back("`]");
    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    return ret;
}
}  // namespace sbe
}  // namespace mongo
