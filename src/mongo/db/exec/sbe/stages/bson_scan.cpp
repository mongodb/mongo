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

#include "mongo/db/exec/sbe/stages/bson_scan.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
BSONScanStage::BSONScanStage(const char* bsonBegin,
                             const char* bsonEnd,
                             boost::optional<value::SlotId> recordSlot,
                             std::vector<std::string> fields,
                             value::SlotVector vars,
                             PlanNodeId planNodeId)
    : PlanStage("bsonscan"_sd, planNodeId),
      _bsonBegin(bsonBegin),
      _bsonEnd(bsonEnd),
      _recordSlot(recordSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)),
      _bsonCurrent(bsonBegin) {}

std::unique_ptr<PlanStage> BSONScanStage::clone() const {
    return std::make_unique<BSONScanStage>(
        _bsonBegin, _bsonEnd, _recordSlot, _fields, _vars, _commonStats.nodeId);
}

void BSONScanStage::prepare(CompileCtx& ctx) {
    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::ViewOfValueAccessor>();
    }

    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        auto [it, inserted] =
            _fieldAccessors.emplace(_fields[idx], std::make_unique<value::ViewOfValueAccessor>());
        uassert(4822841, str::stream() << "duplicate field: " << _fields[idx], inserted);
        auto [itRename, insertedRename] = _varAccessors.emplace(_vars[idx], it->second.get());
        uassert(4822842, str::stream() << "duplicate field: " << _vars[idx], insertedRename);
    }
}

value::SlotAccessor* BSONScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_recordSlot && *_recordSlot == slot) {
        return _recordAccessor.get();
    }

    if (auto it = _varAccessors.find(slot); it != _varAccessors.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void BSONScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _bsonCurrent = _bsonBegin;
}

PlanState BSONScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_bsonCurrent < _bsonEnd) {
        if (_recordAccessor) {
            _recordAccessor->reset(value::TypeTags::bsonObject,
                                   value::bitcastFrom<const char*>(_bsonCurrent));
        }

        if (auto fieldsToMatch = _fieldAccessors.size(); fieldsToMatch != 0) {
            auto be = _bsonCurrent + 4;
            auto end = _bsonCurrent + value::readFromMemory<uint32_t>(_bsonCurrent);
            for (auto& [name, accessor] : _fieldAccessors) {
                accessor->reset();
            }
            while (*be != 0) {
                auto sv = bson::fieldNameView(be);
                if (auto it = _fieldAccessors.find(sv); it != _fieldAccessors.end()) {
                    // Found the field so convert it to Value.
                    auto [tag, val] = bson::convertFrom(true, be, end, sv.size());

                    it->second->reset(tag, val);

                    if ((--fieldsToMatch) == 0) {
                        // No need to scan any further so bail out early.
                        break;
                    }
                }

                be = bson::advance(be, sv.size());
            }
        }

        // Advance to the next document.
        _bsonCurrent += value::readFromMemory<uint32_t>(_bsonCurrent);

        _specificStats.numReads++;
        return trackPlanState(PlanState::ADVANCED);
    }

    _commonStats.isEOF = true;
    return trackPlanState(PlanState::IS_EOF);
}

void BSONScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.closes++;
}

std::unique_ptr<PlanStageStats> BSONScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        if (_recordSlot) {
            bob.appendNumber("recordSlot", static_cast<long long>(*_recordSlot));
        }
        bob.append("field", _fields);
        bob.append("outputSlots", _vars);
        ret->debugInfo = bob.obj();
    }

    return ret;
}

const SpecificStats* BSONScanStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> BSONScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    if (_recordSlot) {
        DebugPrinter::addIdentifier(ret, _recordSlot.get());
    }

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _vars[idx]);
        ret.emplace_back("=");
        DebugPrinter::addIdentifier(ret, _fields[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    return ret;
}
}  // namespace sbe
}  // namespace mongo
