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

#include "mongo/db/exec/sbe/stages/hash_join.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
HashJoinStage::HashJoinStage(std::unique_ptr<PlanStage> outer,
                             std::unique_ptr<PlanStage> inner,
                             value::SlotVector outerCond,
                             value::SlotVector outerProjects,
                             value::SlotVector innerCond,
                             value::SlotVector innerProjects,
                             boost::optional<value::SlotId> collatorSlot,
                             PlanYieldPolicy* yieldPolicy,
                             PlanNodeId planNodeId,
                             bool participateInTrialRunTracking)
    : PlanStage("hj"_sd, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _outerCond(std::move(outerCond)),
      _outerProjects(std::move(outerProjects)),
      _innerCond(std::move(innerCond)),
      _innerProjects(std::move(innerProjects)),
      _collatorSlot(collatorSlot),
      _probeKey(0) {
    if (_outerCond.size() != _innerCond.size()) {
        uasserted(4822823, "left and right size do not match");
    }

    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));
}

std::unique_ptr<PlanStage> HashJoinStage::clone() const {
    return std::make_unique<HashJoinStage>(outerChild()->clone(),
                                           innerChild()->clone(),
                                           _outerCond,
                                           _outerProjects,
                                           _innerCond,
                                           _innerProjects,
                                           _collatorSlot,
                                           _yieldPolicy,
                                           _commonStats.nodeId,
                                           participateInTrialRunTracking());
}

void HashJoinStage::prepare(CompileCtx& ctx) {
    outerChild()->prepare(ctx);
    innerChild()->prepare(ctx);

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(5402502,
                "collator accessor should exist if collator slot provided to HashJoinStage",
                _collatorAccessor != nullptr);
    }

    size_t counter = 0;
    value::SlotSet dupCheck;
    for (auto& slot : _outerCond) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(4822824, str::stream() << "duplicate field: " << slot, inserted);

        _inOuterKeyAccessors.emplace_back(outerChild()->getAccessor(ctx, slot));
    }

    counter = 0;
    for (auto& slot : _innerCond) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(4822825, str::stream() << "duplicate field: " << slot, inserted);

        _inInnerKeyAccessors.emplace_back(innerChild()->getAccessor(ctx, slot));
        _outInnerKeyAccessors.emplace_back(std::make_unique<HashKeyAccessor>(_htIt, counter++));
        _outInnerAccessors[slot] = _outInnerKeyAccessors.back().get();
    }

    counter = 0;
    for (auto& slot : _innerProjects) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(4822826, str::stream() << "duplicate field: " << slot, inserted);

        _inInnerProjectAccessors.emplace_back(innerChild()->getAccessor(ctx, slot));
        _outInnerProjectAccessors.emplace_back(
            std::make_unique<HashProjectAccessor>(_htIt, counter++));
        _outInnerAccessors[slot] = _outInnerProjectAccessors.back().get();
    }

    _probeKey.resize(_inOuterKeyAccessors.size());
}

value::SlotAccessor* HashJoinStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _outInnerAccessors.find(slot); it != _outInnerAccessors.end()) {
        return it->second;
    }
    return outerChild()->getAccessor(ctx, slot);
}

void HashJoinStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    if (_collatorAccessor) {
        auto [tag, collatorVal] = _collatorAccessor->getViewOfValue();
        uassert(5402504, "collatorSlot must be of collator type", tag == value::TypeTags::collator);
        auto collatorView = value::getCollatorView(collatorVal);
        const value::MaterializedRowHasher hasher(collatorView);
        const value::MaterializedRowEq equator(collatorView);
        _ht.emplace(0, hasher, equator);
    } else {
        _ht.emplace();
    }

    _commonStats.opens++;
    innerChild()->open(reOpen);
    // Insert the inner side into the hash table.
    while (innerChild()->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow key{_inInnerKeyAccessors.size()};
        value::MaterializedRow project{_inInnerProjectAccessors.size()};

        size_t idx = 0;
        // Copy keys in order to do the lookup.
        for (auto& p : _inInnerKeyAccessors) {
            key.reset(idx++, p->getCopyOfValue());
        }

        idx = 0;
        // Copy projects.
        for (auto& p : _inInnerProjectAccessors) {
            project.reset(idx++, p->getCopyOfValue());
        }

        _ht->emplace(std::move(key), std::move(project));
    }

    innerChild()->close();
    outerChild()->open(reOpen);

    _htIt = _ht->end();
    _htItEnd = _ht->end();
}

PlanState HashJoinStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    if (_htIt != _htItEnd) {
        ++_htIt;
    }

    if (_htIt == _htItEnd) {
        while (_htIt == _htItEnd) {
            auto state = outerChild()->getNext();
            if (state == PlanState::IS_EOF) {
                // LEFT and OUTER joins should enumerate "non-returned" rows here.
                return trackPlanState(state);
            }

            // Copy keys in order to do the lookup.
            size_t idx = 0;
            for (auto& p : _inOuterKeyAccessors) {
                auto [tag, val] = p->getViewOfValue();
                _probeKey.reset(idx++, false, tag, val);
            }

            auto [low, hi] = _ht->equal_range(_probeKey);
            _htIt = low;
            _htItEnd = hi;
            // If _htIt == _htItEnd (i.e. no match) then RIGHT and OUTER joins
            // should enumerate "non-returned" rows here.
        }
    }

    return trackPlanState(PlanState::ADVANCED);
}

void HashJoinStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    outerChild()->close();
    _ht = boost::none;
}

std::unique_ptr<PlanStageStats> HashJoinStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(outerChild()->getStats(includeDebugInfo));
    ret->children.emplace_back(innerChild()->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* HashJoinStage::getSpecificStats() const {
    return nullptr;
}

void HashJoinStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                 DebugPrintInfo& debugPrintInfo) const {
    if (_collatorSlot) {
        DebugPrinter::addIdentifier(ret, *_collatorSlot);
    }

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    DebugPrinter::addKeyword(ret, "left");

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outerCond.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _outerCond[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outerProjects.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _outerProjects[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, outerChild()->debugPrint(debugPrintInfo));
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    DebugPrinter::addKeyword(ret, "right");
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _innerCond.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _innerCond[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _innerProjects.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _innerProjects[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, innerChild()->debugPrint(debugPrintInfo));
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);
}

size_t HashJoinStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_outerCond);
    size += size_estimator::estimate(_outerProjects);
    size += size_estimator::estimate(_innerCond);
    size += size_estimator::estimate(_innerProjects);
    return size;
}
}  // namespace sbe
}  // namespace mongo
