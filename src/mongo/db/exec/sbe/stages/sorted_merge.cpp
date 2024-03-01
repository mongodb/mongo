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

#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
SortedMergeStage::SortedMergeStage(PlanStage::Vector inputStages,
                                   std::vector<value::SlotVector> inputKeys,
                                   std::vector<value::SortDirection> dirs,
                                   std::vector<value::SlotVector> inputVals,
                                   value::SlotVector outputVals,
                                   PlanNodeId planNodeId,
                                   bool participateInTrialRunTracking)
    : PlanStage("smerge"_sd, nullptr /* yieldPolicy */, planNodeId, participateInTrialRunTracking),
      _inputKeys(std::move(inputKeys)),
      _dirs(std::move(dirs)),
      _inputVals(std::move(inputVals)),
      _outputVals(std::move(outputVals)) {
    _children = std::move(inputStages);

    invariant(_inputKeys.size() == _children.size());
    invariant(_inputVals.size() == _children.size());

    invariant(std::all_of(
        _inputVals.begin(), _inputVals.end(), [size = _outputVals.size()](const auto& slots) {
            return slots.size() == size;
        }));

    invariant(
        std::all_of(_inputKeys.begin(), _inputKeys.end(), [size = _dirs.size()](const auto& slots) {
            return slots.size() == size;
        }));
}

std::unique_ptr<PlanStage> SortedMergeStage::clone() const {
    Vector inputStages;
    inputStages.reserve(_children.size());
    for (auto& child : _children) {
        inputStages.emplace_back(child->clone());
    }
    return std::make_unique<SortedMergeStage>(std::move(inputStages),
                                              _inputKeys,
                                              _dirs,
                                              _inputVals,
                                              _outputVals,
                                              _commonStats.nodeId,
                                              _participateInTrialRunTracking);
}

void SortedMergeStage::prepare(CompileCtx& ctx) {
    std::vector<std::vector<value::SlotAccessor*>> inputKeyAccessors;
    std::vector<PlanStage*> streams;

    for (size_t childNum = 0; childNum < _children.size(); childNum++) {
        auto& child = _children[childNum];
        child->prepare(ctx);

        streams.emplace_back(child.get());

        inputKeyAccessors.emplace_back();

        for (auto slot : _inputKeys[childNum]) {
            inputKeyAccessors.back().push_back(child->getAccessor(ctx, slot));
        }
    }

    for (size_t idx = 0; idx < _outputVals.size(); ++idx) {
        std::vector<value::SlotAccessor*> accessors;
        accessors.reserve(_children.size());

        for (size_t childNum = 0; childNum < _children.size(); childNum++) {
            auto slot = _inputVals[childNum][idx];

            accessors.emplace_back(_children[childNum]->getAccessor(ctx, slot));
        }

        _outAccessors.emplace_back(value::SwitchAccessor{std::move(accessors)});
    }

    _merger.emplace(std::move(inputKeyAccessors), std::move(streams), _dirs, _outAccessors);
}

value::SlotAccessor* SortedMergeStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    for (size_t idx = 0; idx < _outputVals.size(); idx++) {
        if (_outputVals[idx] == slot) {
            return &_outAccessors[idx];
        }
    }

    return ctx.getAccessor(slot);
}

void SortedMergeStage::open(bool reOpen) {
    ++_commonStats.opens;

    for (size_t i = 0; i < _children.size(); ++i) {
        auto& child = _children[i];
        child->open(reOpen);
    }
    _merger->init();
}

PlanState SortedMergeStage::getNext() {
    return trackPlanState(_merger->getNext());
}

void SortedMergeStage::close() {
    trackClose();
    for (auto& child : _children) {
        child->close();
    }

    _merger->clear();
}

std::unique_ptr<PlanStageStats> SortedMergeStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;

        {
            BSONArrayBuilder keysArrBob(bob.subarrayStart("inputKeySlots"));
            for (auto&& slots : _inputKeys) {
                BSONObjBuilder childrenBob(keysArrBob.subobjStart());
                for (size_t idx = 0; idx < slots.size(); ++idx) {
                    childrenBob.append(str::stream() << slots[idx],
                                       _dirs[idx] == sbe::value::SortDirection::Ascending ? "asc"
                                                                                          : "desc");
                }
            }
        }

        {
            BSONArrayBuilder valsArrBob(bob.subarrayStart("inputValSlots"));
            for (auto&& slots : _inputVals) {
                valsArrBob.append(slots.begin(), slots.end());
            }
        }

        bob.append("outputSlots", _outputVals.begin(), _outputVals.end());
        ret->debugInfo = bob.obj();
    }

    for (auto&& child : _children) {
        ret->children.emplace_back(child->getStats(includeDebugInfo));
    }
    return ret;
}

const SpecificStats* SortedMergeStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> SortedMergeStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outputVals.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _outputVals[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _dirs.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret,
                                    _dirs[idx] == value::SortDirection::Ascending ? "asc" : "desc");
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    for (size_t childNum = 0; childNum < _children.size(); childNum++) {
        ret.emplace_back(DebugPrinter::Block("[`"));
        for (size_t idx = 0; idx < _inputKeys[childNum].size(); idx++) {
            if (idx) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }
            DebugPrinter::addIdentifier(ret, _inputKeys[childNum][idx]);
        }
        ret.emplace_back(DebugPrinter::Block("`]"));

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
            ret.emplace_back(DebugPrinter::Block(","));
            DebugPrinter::addNewLine(ret);
        }
    }
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);
    ret.emplace_back(DebugPrinter::Block("`]"));

    return ret;
}

size_t SortedMergeStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_inputKeys);
    size += size_estimator::estimate(_dirs);
    size += size_estimator::estimate(_inputVals);
    size += size_estimator::estimate(_outputVals);
    return size;
}
}  // namespace sbe
}  // namespace mongo
