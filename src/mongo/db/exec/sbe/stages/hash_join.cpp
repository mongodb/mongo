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
#include "mongo/db/exec/sbe/stages/stage_visitors.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
HashJoinStage::HashJoinStage(std::unique_ptr<PlanStage> outer,
                             std::unique_ptr<PlanStage> inner,
                             value::SlotVector outerKey,
                             value::SlotVector outerProjects,
                             value::SlotVector innerKey,
                             value::SlotVector innerProjects,
                             boost::optional<value::SlotId> collatorSlot,
                             PlanYieldPolicy* yieldPolicy,
                             PlanNodeId planNodeId,
                             bool participateInTrialRunTracking)
    : PlanStage("hj"_sd, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _outerKey(std::move(outerKey)),
      _outerProjects(std::move(outerProjects)),
      _innerKey(std::move(innerKey)),
      _innerProjects(std::move(innerProjects)),
      _collatorSlot(collatorSlot) {
    if (_outerKey.size() != _innerKey.size()) {
        uasserted(4822823, "left and right size do not match");
    }

    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));
}

std::unique_ptr<PlanStage> HashJoinStage::clone() const {
    return std::make_unique<HashJoinStage>(outerChild()->clone(),
                                           innerChild()->clone(),
                                           _outerKey,
                                           _outerProjects,
                                           _innerKey,
                                           _innerProjects,
                                           _collatorSlot,
                                           _yieldPolicy,
                                           _commonStats.nodeId,
                                           participateInTrialRunTracking());
}

void HashJoinStage::prepare(CompileCtx& ctx) {
    outerChild()->prepare(ctx);
    innerChild()->prepare(ctx);

    CollatorInterface* collator = nullptr;
    if (_collatorSlot) {
        _collatorAccessor = ctx.getAccessor(*_collatorSlot);
        tassert(5402502,
                "collator accessor should exist if collator slot provided to HashJoinStage",
                _collatorAccessor != nullptr);
        auto [tag, collatorVal] = _collatorAccessor->getViewOfValue();
        uassert(5402504, "collatorSlot must be of collator type", tag == value::TypeTags::collator);
        collator = value::getCollatorView(collatorVal);
    }

    value::SlotSet dupCheck;
    auto setupAccessors = [&](const value::SlotVector& slots,
                              std::vector<value::SlotAccessor*>& inAccessors,
                              std::vector<std::unique_ptr<HashElementAccessor>>& outAccessors,
                              const value::MaterializedRow*& outRow,
                              PlanStage* child) {
        size_t counter = 0;
        for (auto slot : slots) {
            auto [it, inserted] = dupCheck.emplace(slot);
            uassert(4822824, str::stream() << "duplicate field: " << slot, inserted);

            inAccessors.emplace_back(child->getAccessor(ctx, slot));
            outAccessors.emplace_back(std::make_unique<HashElementAccessor>(outRow, counter++));
            _outAccessorMap[slot] = outAccessors.back().get();
        }
    };

    setupAccessors(
        _outerKey, _inOuterKeyAccessors, _outOuterKeyAccessors, _outOuterKeyRow, outerChild());
    setupAccessors(
        _innerKey, _inInnerKeyAccessors, _outInnerKeyAccessors, _outInnerKeyRow, innerChild());
    setupAccessors(_outerProjects,
                   _inOuterProjectAccessors,
                   _outOuterProjectAccessors,
                   _outOuterProjectRow,
                   outerChild());
    setupAccessors(_innerProjects,
                   _inInnerProjectAccessors,
                   _outInnerProjectAccessors,
                   _outInnerProjectRow,
                   innerChild());

    _joinImpl.emplace(
        loadMemoryLimit(StageMemoryLimit::QuerySBEHashJoinApproxMemoryUseInBytesBeforeSpill),
        collator,
        _stats);
}

value::SlotAccessor* HashJoinStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _outAccessorMap.find(slot); it != _outAccessorMap.end()) {
        return it->second;
    }
    return ctx.getAccessor(slot);
}

void HashJoinStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    innerChild()->open(reOpen);

    _joinImpl->reset();

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

        _joinImpl->addBuild(std::move(key), std::move(project));
    }
    _joinImpl->finishBuild();

    innerChild()->close();
    outerChild()->open(reOpen);

    _joinPhase = JoinPhase::kProbing;  // Set initial phase
    _cursor.reset();
}

PlanState HashJoinStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    while (true) {
        if (auto matchResult = _cursor.next(); matchResult) {
            _outInnerKeyRow = matchResult->buildKeyRow;
            _outInnerProjectRow = matchResult->buildProjectRow;
            _outOuterKeyRow = matchResult->probeKeyRow;
            _outOuterProjectRow = matchResult->probeProjectRow;

            return trackPlanState(PlanState::ADVANCED);
        }

        switch (_joinPhase) {
            case JoinPhase::kProbing:
                if (auto state = outerChild()->getNext(); state == PlanState::ADVANCED) {
                    value::MaterializedRow probeKey{_inOuterKeyAccessors.size()};
                    value::MaterializedRow probeProject{_inOuterProjectAccessors.size()};

                    size_t idx = 0;
                    for (auto& p : _inOuterKeyAccessors) {
                        auto [tag, val] = p->getViewOfValue();
                        probeKey.reset(idx++, false, tag, val);
                    }

                    idx = 0;
                    for (auto& p : _inOuterProjectAccessors) {
                        auto [tag, val] = p->getViewOfValue();
                        probeProject.reset(idx++, false, tag, val);
                    }

                    _cursor = _joinImpl->probe(std::move(probeKey), std::move(probeProject));
                    continue;
                }

                // Probe side exhausted, transition to spill processing
                _joinImpl->finishProbe();
                _joinPhase = JoinPhase::kSpillProcessing;
                _cursor.reset();
                [[fallthrough]];
            case JoinPhase::kSpillProcessing:
                if (auto joinCursorOpt = _joinImpl->nextSpilledJoinCursor()) {
                    _cursor = std::move(*joinCursorOpt);
                    continue;
                }
                // No more spilled partitions
                _cursor.reset();
                _joinPhase = JoinPhase::kComplete;
                return trackPlanState(PlanState::IS_EOF);
            case JoinPhase::kComplete:
                return trackPlanState(PlanState::IS_EOF);
            default:
                MONGO_UNREACHABLE;
        }
    }
}

void HashJoinStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    _cursor.reset();
    _joinPhase = JoinPhase::kComplete;
    if (_joinImpl) {
        _joinImpl->reset();
    }

    trackClose();
    outerChild()->close();
}

std::unique_ptr<PlanStageStats> HashJoinStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(outerChild()->getStats(includeDebugInfo));
    ret->children.emplace_back(innerChild()->getStats(includeDebugInfo));

    const HashJoinStats* specificStats = static_cast<const HashJoinStats*>(getSpecificStats());
    ret->specific = std::make_unique<HashJoinStats>(*specificStats);
    if (includeDebugInfo) {
        BSONObjBuilder bob(StorageAccessStatsVisitor::collectStats(*this, *ret).toBSON());
        // Spilling stats.
        auto& spillingStats = specificStats->spillingStats;
        bob.appendBool("usedDisk", specificStats->usedDisk)
            .appendNumber("numPartitionsSpilled", specificStats->numPartitionsSpilled)
            .appendNumber("numPartitionSwaps", specificStats->numPartitionSwaps)
            .appendNumber("recursionDepthMax", specificStats->recursionDepthMax)
            .appendNumber("spills", static_cast<long long>(spillingStats.getSpills()))
            .appendNumber("spilledRecords",
                          static_cast<long long>(spillingStats.getSpilledRecords()))
            .appendNumber("spilledBytes", static_cast<long long>(spillingStats.getSpilledBytes()))
            .appendNumber("spilledDataStorageSize",
                          static_cast<long long>(spillingStats.getSpilledDataStorageSize()));
        if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
            bob.appendNumber("peakTrackedMemBytes",
                             static_cast<long long>(specificStats->peakTrackedMemBytes));
        }
        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* HashJoinStage::getSpecificStats() const {
    return &_stats;
}

void HashJoinStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                 DebugPrintInfo& debugPrintInfo) const {
    if (_collatorSlot) {
        DebugPrinter::addIdentifier(ret, *_collatorSlot);
    }

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    DebugPrinter::addKeyword(ret, "left");

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outerKey.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _outerKey[idx]);
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
    for (size_t idx = 0; idx < _innerKey.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _innerKey[idx]);
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
    size += size_estimator::estimate(_outerKey);
    size += size_estimator::estimate(_outerProjects);
    size += size_estimator::estimate(_innerKey);
    size += size_estimator::estimate(_innerProjects);
    return size;
}

void HashJoinStage::doSaveState() {
    _cursor.saveState();
}
}  // namespace sbe
}  // namespace mongo
