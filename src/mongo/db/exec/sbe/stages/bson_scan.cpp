// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/bson_scan.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
using namespace std::literals::string_view_literals;
BSONScanStage::BSONScanStage(std::vector<BSONObj> bsons,
                             boost::optional<value::SlotId> recordSlot,
                             PlanNodeId planNodeId,
                             std::vector<std::string> scanFieldNames,
                             value::SlotVector scanFieldSlots,
                             bool participateInTrialRunTracking)
    : PlanStage("bsonscan"sv, nullptr /* yieldPolicy */, planNodeId, participateInTrialRunTracking),
      _bsons(std::move(bsons)),
      _recordSlot(recordSlot),
      _scanFieldNames(std::move(scanFieldNames)),
      _scanFieldSlots(std::move(scanFieldSlots)) {
    _bsonCurrent = _bsons.begin();
}

std::unique_ptr<PlanStage> BSONScanStage::clone() const {
    return std::make_unique<BSONScanStage>(_bsons,
                                           _recordSlot,
                                           _commonStats.nodeId,
                                           _scanFieldNames,
                                           _scanFieldSlots,
                                           participateInTrialRunTracking());
}

void BSONScanStage::prepare(CompileCtx& ctx) {
    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::ViewOfValueAccessor>();
    }

    for (size_t idx = 0; idx < _scanFieldNames.size(); ++idx) {
        auto [it, inserted] = _scanFieldAccessors.emplace(
            _scanFieldNames[idx], std::make_unique<value::ViewOfValueAccessor>());
        uassert(4822841, str::stream() << "duplicate field: " << _scanFieldNames[idx], inserted);
        auto [itRename, insertedRename] =
            _scanFieldAccessorsMap.emplace(_scanFieldSlots[idx], it->second.get());
        uassert(
            4822842, str::stream() << "duplicate field: " << _scanFieldSlots[idx], insertedRename);
    }
}

value::SlotAccessor* BSONScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_recordSlot && *_recordSlot == slot) {
        return _recordAccessor.get();
    }

    if (auto it = _scanFieldAccessorsMap.find(slot); it != _scanFieldAccessorsMap.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void BSONScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _bsonCurrent = _bsons.begin();
}

PlanState BSONScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_bsonCurrent != _bsons.end()) {
        if (_recordAccessor) {
            _recordAccessor->reset(value::TypeTags::bsonObject,
                                   value::bitcastFrom<const char*>(_bsonCurrent->objdata()));
        }

        if (auto fieldsToMatch = _scanFieldAccessors.size(); fieldsToMatch != 0) {
            for (auto& [name, accessor] : _scanFieldAccessors) {
                accessor->reset();
            }
            for (const auto& element : *_bsonCurrent) {
                auto fieldName = element.fieldNameStringData();
                if (auto it = _scanFieldAccessors.find(fieldName);
                    it != _scanFieldAccessors.end()) {
                    // Found the field so convert it to Value.
                    auto [tag, val] = bson::convertToView(element);
                    it->second->reset(tag, val);

                    if ((--fieldsToMatch) == 0) {
                        // No need to scan any further so bail out early.
                        break;
                    }
                }
            }
        }

        // Advance to the next document.
        ++_bsonCurrent;

        _specificStats.numReads++;
        return trackPlanState(PlanState::ADVANCED);
    }

    return trackPlanState(PlanState::IS_EOF);
}

void BSONScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
}

std::unique_ptr<PlanStageStats> BSONScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        if (_recordSlot) {
            bob.appendNumber("recordSlot", static_cast<long long>(*_recordSlot));
        }
        bob.append("field", _scanFieldNames);
        bob.append("outputSlots", _scanFieldSlots.begin(), _scanFieldSlots.end());
        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* BSONScanStage::getSpecificStats() const {
    return &_specificStats;
}

void BSONScanStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                 DebugPrintInfo& debugPrintInfo) const {
    if (_recordSlot) {
        DebugPrinter::addIdentifier(ret, _recordSlot.value());
    }

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _scanFieldNames.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _scanFieldSlots[idx]);
        ret.emplace_back("=");
        DebugPrinter::addIdentifier(ret, _scanFieldNames[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));
}

size_t BSONScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_scanFieldNames);
    size += size_estimator::estimate(_scanFieldSlots);
    return size;
}

}  // namespace sbe
}  // namespace mongo
