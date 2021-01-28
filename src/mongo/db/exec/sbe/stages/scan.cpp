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

#include "mongo/db/exec/sbe/stages/scan.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
ScanStage::ScanStage(const NamespaceStringOrUUID& name,
                     boost::optional<value::SlotId> recordSlot,
                     boost::optional<value::SlotId> recordIdSlot,
                     std::vector<std::string> fields,
                     value::SlotVector vars,
                     boost::optional<value::SlotId> seekKeySlot,
                     bool forward,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     LockAcquisitionCallback lockAcquisitionCallback,
                     ScanOpenCallback openCallback)
    : PlanStage(seekKeySlot ? "seek"_sd : "scan"_sd, yieldPolicy, nodeId),
      _name(name),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)),
      _seekKeySlot(seekKeySlot),
      _forward(forward),
      _lockAcquisitionCallback(std::move(lockAcquisitionCallback)),
      _openCallback(openCallback) {
    invariant(_fields.size() == _vars.size());
    invariant(!_seekKeySlot || _forward);
}

std::unique_ptr<PlanStage> ScanStage::clone() const {
    return std::make_unique<ScanStage>(_name,
                                       _recordSlot,
                                       _recordIdSlot,
                                       _fields,
                                       _vars,
                                       _seekKeySlot,
                                       _forward,
                                       _yieldPolicy,
                                       _commonStats.nodeId,
                                       _lockAcquisitionCallback,
                                       _openCallback);
}

void ScanStage::prepare(CompileCtx& ctx) {
    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::ViewOfValueAccessor>();
    }

    if (_recordIdSlot) {
        _recordIdAccessor = std::make_unique<value::ViewOfValueAccessor>();
    }

    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        auto [it, inserted] =
            _fieldAccessors.emplace(_fields[idx], std::make_unique<value::ViewOfValueAccessor>());
        uassert(4822814, str::stream() << "duplicate field: " << _fields[idx], inserted);
        auto [itRename, insertedRename] = _varAccessors.emplace(_vars[idx], it->second.get());
        uassert(4822815, str::stream() << "duplicate field: " << _vars[idx], insertedRename);
    }

    if (_seekKeySlot) {
        _seekKeyAccessor = ctx.getAccessor(*_seekKeySlot);
    }
}

value::SlotAccessor* ScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_recordSlot && *_recordSlot == slot) {
        return _recordAccessor.get();
    }

    if (_recordIdSlot && *_recordIdSlot == slot) {
        return _recordIdAccessor.get();
    }

    if (auto it = _varAccessors.find(slot); it != _varAccessors.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void ScanStage::doSaveState() {
    if (_cursor) {
        _cursor->save();
    }

    _coll.reset();
}

void ScanStage::doRestoreState() {
    invariant(_opCtx);
    invariant(!_coll);

    // If this stage is not currently open, then there is nothing to restore.
    if (!_open) {
        return;
    }

    _coll.emplace(_opCtx, _name);
    if (_lockAcquisitionCallback) {
        _lockAcquisitionCallback(_opCtx, *_coll);
    }

    if (_cursor) {
        const bool couldRestore = _cursor->restore();
        uassert(ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. ",
                couldRestore);
    }
}

void ScanStage::doDetachFromOperationContext() {
    if (_cursor) {
        _cursor->detachFromOperationContext();
    }
}

void ScanStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_cursor) {
        _cursor->reattachToOperationContext(opCtx);
    }
}

void ScanStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

void ScanStage::doAttachToTrialRunTracker(TrialRunTracker* tracker) {
    _tracker = tracker;
}

void ScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    invariant(_opCtx);
    if (!reOpen) {
        invariant(!_cursor);
        invariant(!_coll);
        _coll.emplace(_opCtx, _name);
        if (_lockAcquisitionCallback) {
            _lockAcquisitionCallback(_opCtx, *_coll);
        }
    } else {
        invariant(_cursor);
        invariant(_coll);
    }

    // TODO: this is currently used only to wait for oplog entries to become visible, so we
    // may want to consider to move this logic into storage API instead.
    if (_openCallback) {
        _openCallback(_opCtx, _coll->getCollection(), reOpen);
    }

    if (const auto& collection = _coll->getCollection()) {
        if (_seekKeyAccessor) {
            auto [tag, val] = _seekKeyAccessor->getViewOfValue();
            const auto msgTag = tag;
            uassert(ErrorCodes::BadValue,
                    str::stream() << "seek key is wrong type: " << msgTag,
                    tag == value::TypeTags::RecordId);

            _key = RecordId{value::bitcastTo<int64_t>(val)};
        }

        if (!_cursor || !_seekKeyAccessor) {
            _cursor = collection->getCursor(_opCtx, _forward);
        }
    } else {
        _cursor.reset();
    }

    _open = true;
    _firstGetNext = true;
}

PlanState ScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (!_cursor) {
        return trackPlanState(PlanState::IS_EOF);
    }

    checkForInterrupt(_opCtx);

    auto nextRecord =
        (_firstGetNext && _seekKeyAccessor) ? _cursor->seekExact(_key) : _cursor->next();
    _firstGetNext = false;

    if (!nextRecord) {
        return trackPlanState(PlanState::IS_EOF);
    }

    if (_recordAccessor) {
        _recordAccessor->reset(value::TypeTags::bsonObject,
                               value::bitcastFrom<const char*>(nextRecord->data.data()));
    }

    if (_recordIdAccessor) {
        _recordIdAccessor->reset(value::TypeTags::RecordId,
                                 value::bitcastFrom<int64_t>(nextRecord->id.as<int64_t>()));
    }

    if (!_fieldAccessors.empty()) {
        auto fieldsToMatch = _fieldAccessors.size();
        auto rawBson = nextRecord->data.data();
        auto be = rawBson + 4;
        auto end = rawBson + ConstDataView(rawBson).read<LittleEndian<uint32_t>>();
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

    if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumReads>(1)) {
        // If we're collecting execution stats during multi-planning and reached the end of the
        // trial period (trackProgress() will return 'true' in this case), then we can reset the
        // tracker. Note that a trial period is executed only once per a PlanStge tree, and once
        // completed never run again on the same tree.
        _tracker = nullptr;
    }
    ++_specificStats.numReads;
    return trackPlanState(PlanState::ADVANCED);
}

void ScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.closes++;
    _cursor.reset();
    _coll.reset();
    _open = false;
}

std::unique_ptr<PlanStageStats> ScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("numReads", _specificStats.numReads);
        if (_recordSlot) {
            bob.appendIntOrLL("recordSlot", *_recordSlot);
        }
        if (_recordIdSlot) {
            bob.appendIntOrLL("recordIdSlot", *_recordIdSlot);
        }
        if (_seekKeySlot) {
            bob.appendIntOrLL("seekKeySlot", *_seekKeySlot);
        }
        bob.append("fields", _fields);
        bob.append("outputSlots", _vars);
        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* ScanStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> ScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    if (_seekKeySlot) {
        DebugPrinter::addIdentifier(ret, _seekKeySlot.get());
    }

    if (_recordSlot) {
        DebugPrinter::addIdentifier(ret, _recordSlot.get());
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.get());
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

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _name.toString());
    ret.emplace_back("`\"");

    return ret;
}

ParallelScanStage::ParallelScanStage(const NamespaceStringOrUUID& name,
                                     boost::optional<value::SlotId> recordSlot,
                                     boost::optional<value::SlotId> recordIdSlot,
                                     std::vector<std::string> fields,
                                     value::SlotVector vars,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId nodeId)
    : PlanStage("pscan"_sd, yieldPolicy, nodeId),
      _name(name),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)) {
    invariant(_fields.size() == _vars.size());

    _state = std::make_shared<ParallelState>();
}

ParallelScanStage::ParallelScanStage(const std::shared_ptr<ParallelState>& state,
                                     const NamespaceStringOrUUID& name,
                                     boost::optional<value::SlotId> recordSlot,
                                     boost::optional<value::SlotId> recordIdSlot,
                                     std::vector<std::string> fields,
                                     value::SlotVector vars,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId nodeId)
    : PlanStage("pscan"_sd, yieldPolicy, nodeId),
      _name(name),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)),
      _state(state) {
    invariant(_fields.size() == _vars.size());
}

std::unique_ptr<PlanStage> ParallelScanStage::clone() const {
    return std::make_unique<ParallelScanStage>(_state,
                                               _name,
                                               _recordSlot,
                                               _recordIdSlot,
                                               _fields,
                                               _vars,
                                               _yieldPolicy,
                                               _commonStats.nodeId);
}

void ParallelScanStage::prepare(CompileCtx& ctx) {
    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::ViewOfValueAccessor>();
    }

    if (_recordIdSlot) {
        _recordIdAccessor = std::make_unique<value::ViewOfValueAccessor>();
    }

    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        auto [it, inserted] =
            _fieldAccessors.emplace(_fields[idx], std::make_unique<value::ViewOfValueAccessor>());
        uassert(4822816, str::stream() << "duplicate field: " << _fields[idx], inserted);
        auto [itRename, insertedRename] = _varAccessors.emplace(_vars[idx], it->second.get());
        uassert(4822817, str::stream() << "duplicate field: " << _vars[idx], insertedRename);
    }
}

value::SlotAccessor* ParallelScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_recordSlot && *_recordSlot == slot) {
        return _recordAccessor.get();
    }

    if (_recordIdSlot && *_recordIdSlot == slot) {
        return _recordIdAccessor.get();
    }

    if (auto it = _varAccessors.find(slot); it != _varAccessors.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void ParallelScanStage::doSaveState() {
    if (_cursor) {
        _cursor->save();
    }

    _coll.reset();
}

void ParallelScanStage::doRestoreState() {
    invariant(_opCtx);
    invariant(!_coll);

    // If this stage is not currently open, then there is nothing to restore.
    if (!_open) {
        return;
    }

    _coll.emplace(_opCtx, _name);

    uassertStatusOK(repl::ReplicationCoordinator::get(_opCtx)->checkCanServeReadsFor(
        _opCtx, _coll->getNss(), true));

    if (_cursor) {
        const bool couldRestore = _cursor->restore();
        uassert(ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. ",
                couldRestore);
    }
}

void ParallelScanStage::doDetachFromOperationContext() {
    if (_cursor) {
        _cursor->detachFromOperationContext();
    }
}

void ParallelScanStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_cursor) {
        _cursor->reattachToOperationContext(opCtx);
    }
}

void ParallelScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    invariant(_opCtx);
    invariant(!reOpen, "parallel scan is not restartable");

    invariant(!_cursor);
    invariant(!_coll);
    _coll.emplace(_opCtx, _name);

    uassertStatusOK(repl::ReplicationCoordinator::get(_opCtx)->checkCanServeReadsFor(
        _opCtx, _coll->getNss(), true));

    const auto& collection = _coll->getCollection();

    if (collection) {
        {
            stdx::unique_lock lock(_state->mutex);
            if (_state->ranges.empty()) {
                auto ranges = collection->getRecordStore()->numRecords(_opCtx) / 10240;
                if (ranges < 2) {
                    _state->ranges.emplace_back(Range{RecordId{}, RecordId{}});
                } else {
                    if (ranges > 1024) {
                        ranges = 1024;
                    }
                    auto randomCursor = collection->getRecordStore()->getRandomCursor(_opCtx);
                    invariant(randomCursor);
                    std::set<RecordId> rids;
                    while (ranges--) {
                        auto nextRecord = randomCursor->next();
                        if (nextRecord) {
                            rids.emplace(nextRecord->id);
                        }
                    }
                    RecordId lastid{};
                    for (auto id : rids) {
                        _state->ranges.emplace_back(Range{lastid, id});
                        lastid = id;
                    }
                    _state->ranges.emplace_back(Range{lastid, RecordId{}});
                }
            }
        }

        _cursor = collection->getCursor(_opCtx);
    }

    _open = true;
}

boost::optional<Record> ParallelScanStage::nextRange() {
    invariant(_cursor);
    _currentRange = _state->currentRange.fetchAndAdd(1);
    if (_currentRange < _state->ranges.size()) {
        _range = _state->ranges[_currentRange];

        return _range.begin.isNull() ? _cursor->next() : _cursor->seekExact(_range.begin);
    } else {
        return boost::none;
    }
}

PlanState ParallelScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (!_cursor) {
        _commonStats.isEOF = true;
        return PlanState::IS_EOF;
    }

    checkForInterrupt(_opCtx);

    boost::optional<Record> nextRecord;

    do {
        nextRecord = needsRange() ? nextRange() : _cursor->next();
        if (!nextRecord) {
            _commonStats.isEOF = true;
            return PlanState::IS_EOF;
        }

        if (!_range.end.isNull() && nextRecord->id == _range.end) {
            setNeedsRange();
            nextRecord = boost::none;
        }
    } while (!nextRecord);

    if (_recordAccessor) {
        _recordAccessor->reset(value::TypeTags::bsonObject,
                               value::bitcastFrom<const char*>(nextRecord->data.data()));
    }

    if (_recordIdAccessor) {
        _recordIdAccessor->reset(value::TypeTags::RecordId,
                                 value::bitcastFrom<int64_t>(nextRecord->id.as<int64_t>()));
    }


    if (!_fieldAccessors.empty()) {
        auto fieldsToMatch = _fieldAccessors.size();
        auto rawBson = nextRecord->data.data();
        auto be = rawBson + 4;
        auto end = rawBson + ConstDataView(rawBson).read<LittleEndian<uint32_t>>();
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

    return PlanState::ADVANCED;
}

void ParallelScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    _cursor.reset();
    _coll.reset();
    _open = false;
}

std::unique_ptr<PlanStageStats> ParallelScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    return ret;
}

const SpecificStats* ParallelScanStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> ParallelScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    if (_recordSlot) {
        DebugPrinter::addIdentifier(ret, _recordSlot.get());
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.get());
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

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _name.toString());
    ret.emplace_back("`\"");

    return ret;
}
}  // namespace sbe
}  // namespace mongo
