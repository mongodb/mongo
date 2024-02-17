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
#include <absl/container/node_hash_map.h>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/unique.h"

namespace mongo {
namespace sbe {
UniqueStage::UniqueStage(std::unique_ptr<PlanStage> input,
                         value::SlotVector keys,
                         PlanNodeId planNodeId,
                         bool participateInTrialRunTracking)
    : PlanStage("unique"_sd, nullptr /* yieldPolicy */, planNodeId, participateInTrialRunTracking),
      _keySlots(keys) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> UniqueStage::clone() const {
    return std::make_unique<UniqueStage>(
        _children[0]->clone(), _keySlots, _commonStats.nodeId, _participateInTrialRunTracking);
}

void UniqueStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
    for (auto&& keySlot : _keySlots) {
        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, keySlot));
    }
}

value::SlotAccessor* UniqueStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return _children[0]->getAccessor(ctx, slot);
}

void UniqueStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    ++_commonStats.opens;

    if (reOpen) {
        _seen.clear();
    }
    _children[0]->open(reOpen);
}

PlanState UniqueStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    while (_children[0]->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow key{_inKeyAccessors.size()};
        size_t idx = 0;
        for (auto& accessor : _inKeyAccessors) {
            auto [tag, val] = accessor->getViewOfValue();
            key.reset(idx++, false, tag, val);
        }

        ++_specificStats.dupsTested;
        auto [it, inserted] = _seen.emplace(std::move(key));
        if (inserted) {
            const_cast<value::MaterializedRow&>(*it).makeOwned();
            return trackPlanState(PlanState::ADVANCED);
        } else {
            // This row has been seen already, so we skip it.
            ++_specificStats.dupsDropped;
        }
    }

    return trackPlanState(PlanState::IS_EOF);
}

void UniqueStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();

    _seen.clear();
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> UniqueStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<UniqueStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("dupsTested", static_cast<long long>(_specificStats.dupsTested));
        bob.appendNumber("dupsDropped", static_cast<long long>(_specificStats.dupsDropped));
        bob.append("keySlots", _keySlots.begin(), _keySlots.end());
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* UniqueStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> UniqueStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _keySlots.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _keySlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

size_t UniqueStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size_estimator::estimate(_keySlots);
    size += size_estimator::estimate(_specificStats);
    return size;
}

}  // namespace sbe
}  // namespace mongo
