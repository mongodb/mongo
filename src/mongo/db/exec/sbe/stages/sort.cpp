// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/sort.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>


namespace mongo::sbe {
using namespace std::literals::string_view_literals;
SortStage::SortStage(std::unique_ptr<PlanStage> input,
                     value::SlotVector obs,
                     std::vector<value::SortDirection> dirs,
                     value::SlotVector vals,
                     std::unique_ptr<EExpression> limit,
                     size_t memoryLimit,
                     bool allowDiskUse,
                     PlanYieldPolicySBE* yieldPolicy,
                     PlanNodeId planNodeId,
                     bool participateInTrialRunTracking)
    : PlanStage("sort"sv, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _obs(std::move(obs)),
      _dirs(std::move(dirs)),
      _vals(std::move(vals)),
      _allowDiskUse(allowDiskUse),
      _limitExpr(std::move(limit)) {
    _children.emplace_back(std::move(input));

    tassert(11094711,
            "Expect the number of order-by slots to match the number of sort directions",
            _obs.size() == _dirs.size());

    _specificStats.maxMemoryUsageBytes = memoryLimit;
}

SortStage::~SortStage() {}

void SortStage::prepare(CompileCtx& ctx) {
    _stageImpl = _makeStageImpl();
    _stageImpl->prepare(ctx);
}

value::SlotAccessor* SortStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return _stageImpl->getAccessor(ctx, slot);
}

void SortStage::open(bool reOpen) {
    _stageImpl->open(reOpen);
}

PlanState SortStage::getNext() {
    return _stageImpl->getNext();
}

void SortStage::close() {
    return _stageImpl->close();
}

std::unique_ptr<PlanStage> SortStage::clone() const {
    return std::make_unique<SortStage>(_children[0]->clone(),
                                       _obs,
                                       _dirs,
                                       _vals,
                                       _limitExpr ? _limitExpr->clone() : nullptr,
                                       _specificStats.maxMemoryUsageBytes,
                                       _allowDiskUse,
                                       _yieldPolicy,
                                       _commonStats.nodeId,
                                       participateInTrialRunTracking());
}

std::unique_ptr<PlanStageStats> SortStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<SortStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("memLimit", static_cast<long long>(_specificStats.maxMemoryUsageBytes));
        bob.appendNumber("totalDataSizeSorted",
                         static_cast<long long>(_specificStats.totalDataSizeBytes));
        bob.appendBool("usedDisk", _specificStats.spillingStats.getSpills() > 0);
        bob.appendNumber("spills",
                         static_cast<long long>(_specificStats.spillingStats.getSpills()));
        bob.appendNumber(
            "spilledDataStorageSize",
            static_cast<long long>(_specificStats.spillingStats.getSpilledDataStorageSize()));
        bob.appendNumber("spilledBytes",
                         static_cast<long long>(_specificStats.spillingStats.getSpilledBytes()));
        bob.appendNumber("spilledRecords",
                         static_cast<long long>(_specificStats.spillingStats.getSpilledRecords()));
        if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
            bob.appendNumber("peakTrackedMemBytes",
                             static_cast<long long>(_specificStats.peakTrackedMemBytes));
        }

        BSONObjBuilder childrenBob(bob.subobjStart("orderBySlots"));
        for (size_t idx = 0; idx < _obs.size(); ++idx) {
            childrenBob.append(str::stream() << _obs[idx],
                               _dirs[idx] == sbe::value::SortDirection::Ascending ? "asc" : "desc");
        }
        childrenBob.doneFast();
        bob.append("outputSlots", _vals.begin(), _vals.end());
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* SortStage::getSpecificStats() const {
    return &_specificStats;
}

void SortStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                             DebugPrintInfo& debugPrintInfo) const {
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _obs.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _obs[idx]);
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
    for (size_t idx = 0; idx < _vals.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _vals[idx]);
    }
    ret.emplace_back("`]");

    if (_limitExpr) {
        DebugPrinter::addBlocks(ret, _limitExpr->debugPrint());
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t SortStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_obs);
    size += size_estimator::estimate(_dirs);
    size += size_estimator::estimate(_vals);
    size += size_estimator::estimate(_specificStats);
    size += _limitExpr ? _limitExpr->estimateSize() : 0;
    return size;
}

}  // namespace mongo::sbe
