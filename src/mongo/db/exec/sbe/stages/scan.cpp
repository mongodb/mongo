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
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
ScanStage::ScanStage(CollectionUUID collectionUuid,
                     boost::optional<value::SlotId> recordSlot,
                     boost::optional<value::SlotId> recordIdSlot,
                     boost::optional<value::SlotId> snapshotIdSlot,
                     boost::optional<value::SlotId> indexIdSlot,
                     boost::optional<value::SlotId> indexKeySlot,
                     boost::optional<value::SlotId> indexKeyPatternSlot,
                     boost::optional<value::SlotId> oplogTsSlot,
                     std::vector<std::string> fields,
                     value::SlotVector vars,
                     boost::optional<value::SlotId> seekKeySlot,
                     bool forward,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     ScanCallbacks scanCallbacks)
    : PlanStage(seekKeySlot ? "seek"_sd : "scan"_sd, yieldPolicy, nodeId),
      _collUuid(collectionUuid),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdSlot(indexIdSlot),
      _indexKeySlot(indexKeySlot),
      _indexKeyPatternSlot(indexKeyPatternSlot),
      _oplogTsSlot(oplogTsSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)),
      _seekKeySlot(seekKeySlot),
      _forward(forward),
      _scanCallbacks(std::move(scanCallbacks)) {
    invariant(_fields.size() == _vars.size());
    invariant(!_seekKeySlot || _forward);
    tassert(5567202,
            "The '_oplogTsSlot' cannot be set without 'ts' field in '_fields'",
            !_oplogTsSlot ||
                (std::find(_fields.begin(), _fields.end(), repl::OpTime::kTimestampFieldName) !=
                 _fields.end()));
}

std::unique_ptr<PlanStage> ScanStage::clone() const {
    return std::make_unique<ScanStage>(_collUuid,
                                       _recordSlot,
                                       _recordIdSlot,
                                       _snapshotIdSlot,
                                       _indexIdSlot,
                                       _indexKeySlot,
                                       _indexKeyPatternSlot,
                                       _oplogTsSlot,
                                       _fields,
                                       _vars,
                                       _seekKeySlot,
                                       _forward,
                                       _yieldPolicy,
                                       _commonStats.nodeId,
                                       _scanCallbacks);
}

void ScanStage::prepare(CompileCtx& ctx) {
    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    if (_recordIdSlot) {
        _recordIdAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        auto [it, inserted] =
            _fieldAccessors.emplace(_fields[idx], std::make_unique<value::OwnedValueAccessor>());
        uassert(4822814, str::stream() << "duplicate field: " << _fields[idx], inserted);
        auto [itRename, insertedRename] = _varAccessors.emplace(_vars[idx], it->second.get());
        uassert(4822815, str::stream() << "duplicate field: " << _vars[idx], insertedRename);
    }

    if (_seekKeySlot) {
        _seekKeyAccessor = ctx.getAccessor(*_seekKeySlot);
    }

    if (_snapshotIdSlot) {
        _snapshotIdAccessor = ctx.getAccessor(*_snapshotIdSlot);
    }

    if (_indexIdSlot) {
        _indexIdAccessor = ctx.getAccessor(*_indexIdSlot);
    }

    if (_indexKeySlot) {
        _indexKeyAccessor = ctx.getAccessor(*_indexKeySlot);
    }

    if (_indexKeyPatternSlot) {
        _indexKeyPatternAccessor = ctx.getAccessor(*_indexKeyPatternSlot);
    }

    if (_oplogTsSlot) {
        _oplogTsAccessor = ctx.getRuntimeEnvAccessor(*_oplogTsSlot);
    }

    tassert(5709600, "'_coll' should not be initialized prior to 'acquireCollection()'", !_coll);
    std::tie(_coll, _collName, _catalogEpoch) = acquireCollection(_opCtx, _collUuid);
}

value::SlotAccessor* ScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_recordSlot && *_recordSlot == slot) {
        return _recordAccessor.get();
    }

    if (_recordIdSlot && *_recordIdSlot == slot) {
        return _recordIdAccessor.get();
    }

    if (_oplogTsSlot && *_oplogTsSlot == slot) {
        return _oplogTsAccessor;
    }

    if (auto it = _varAccessors.find(slot); it != _varAccessors.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void ScanStage::doSaveState() {
    if (slotsAccessible()) {
        if (_recordAccessor) {
            _recordAccessor->makeOwned();
        }
        if (_recordIdAccessor) {
            _recordIdAccessor->makeOwned();
        }
        for (auto& [fieldName, accessor] : _fieldAccessors) {
            accessor->makeOwned();
        }
    }

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

    _coll = restoreCollection(_opCtx, _collName, _collUuid, _catalogEpoch);

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

    if (_open) {
        tassert(5071001, "reopened ScanStage but reOpen=false", reOpen);
        tassert(5071002, "ScanStage is open but _coll is not null", _coll);
        tassert(5071003, "ScanStage is open but don't have _cursor", _cursor);
    } else {
        tassert(5071004, "first open to ScanStage but reOpen=true", !reOpen);
        if (!_coll) {
            // We're being opened after 'close()'. We need to re-acquire '_coll' in this case and
            // make some validity checks (the collection has not been dropped, renamed, etc.).
            tassert(5071005, "ScanStage is not open but have _cursor", !_cursor);
            _coll = restoreCollection(_opCtx, _collName, _collUuid, _catalogEpoch);
        }
    }

    if (_scanCallbacks.scanOpenCallback) {
        _scanCallbacks.scanOpenCallback(_opCtx, _coll, reOpen);
    }

    if (_coll) {
        if (_seekKeyAccessor) {
            auto [tag, val] = _seekKeyAccessor->getViewOfValue();
            const auto msgTag = tag;
            uassert(ErrorCodes::BadValue,
                    str::stream() << "seek key is wrong type: " << msgTag,
                    tag == value::TypeTags::RecordId);

            _key = RecordId{value::bitcastTo<int64_t>(val)};
        }

        if (!_cursor || !_seekKeyAccessor) {
            _cursor = _coll->getCursor(_opCtx, _forward);
        }
    } else {
        _cursor.reset();
    }

    _open = true;
    _firstGetNext = true;
}

PlanState ScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call next() on a storage cursor so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the next() call.
    disableSlotAccess();

    if (!_cursor) {
        return trackPlanState(PlanState::IS_EOF);
    }

    checkForInterrupt(_opCtx);

    auto res = _firstGetNext && _seekKeyAccessor;
    auto nextRecord = res ? _cursor->seekExact(_key) : _cursor->next();
    _firstGetNext = false;

    if (!nextRecord) {
        // Only check our index key for corruption during the first call to 'getNext' and while
        // seeking.
        if (_scanCallbacks.indexKeyCorruptionCheckCallback) {
            tassert(5113712,
                    "Index key corruption check can only be performed on the first call "
                    "to getNext() during a seek",
                    res);
            _scanCallbacks.indexKeyCorruptionCheckCallback(_opCtx,
                                                           _snapshotIdAccessor,
                                                           _indexKeyAccessor,
                                                           _indexKeyPatternAccessor,
                                                           _key,
                                                           _collName);
        }
        return trackPlanState(PlanState::IS_EOF);
    }

    // Return EOF if the index key is found to be inconsistent.
    if (_scanCallbacks.indexKeyConsistencyCheckCallBack &&
        !_scanCallbacks.indexKeyConsistencyCheckCallBack(
            _opCtx, _snapshotIdAccessor, _indexIdAccessor, _indexKeyAccessor, *nextRecord)) {
        return trackPlanState(PlanState::IS_EOF);
    }

    if (_recordAccessor) {
        _recordAccessor->reset(false,
                               value::TypeTags::bsonObject,
                               value::bitcastFrom<const char*>(nextRecord->data.data()));
    }

    if (_recordIdAccessor) {
        _recordIdAccessor->reset(false,
                                 value::TypeTags::RecordId,
                                 value::bitcastFrom<int64_t>(nextRecord->id.getLong()));
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
                auto [tag, val] = bson::convertFrom<true>(be, end, sv.size());

                if (_oplogTsAccessor && it->first == repl::OpTime::kTimestampFieldName) {
                    auto&& [ownedTag, ownedVal] = value::copyValue(tag, val);
                    _oplogTsAccessor->reset(false, ownedTag, ownedVal);
                }

                it->second->reset(false, tag, val);

                if ((--fieldsToMatch) == 0) {
                    // No need to scan any further so bail out early.
                    break;
                }
            }

            be = bson::advance(be, sv.size());
        }
    }

    ++_specificStats.numReads;
    if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumReads>(1)) {
        // If we're collecting execution stats during multi-planning and reached the end of the
        // trial period because we've performed enough physical reads, bail out from the trial run
        // by raising a special exception to signal a runtime planner that this candidate plan has
        // completed its trial run early. Note that a trial period is executed only once per a
        // PlanStage tree, and once completed never run again on the same tree.
        _tracker = nullptr;
        uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in scan");
    }
    return trackPlanState(PlanState::ADVANCED);
}

void ScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _cursor.reset();
    _coll.reset();
    _open = false;
}

std::unique_ptr<PlanStageStats> ScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("numReads", static_cast<long long>(_specificStats.numReads));
        if (_recordSlot) {
            bob.appendNumber("recordSlot", static_cast<long long>(*_recordSlot));
        }
        if (_recordIdSlot) {
            bob.appendNumber("recordIdSlot", static_cast<long long>(*_recordIdSlot));
        }
        if (_seekKeySlot) {
            bob.appendNumber("seekKeySlot", static_cast<long long>(*_seekKeySlot));
        }
        if (_snapshotIdSlot) {
            bob.appendNumber("snapshotIdSlot", static_cast<long long>(*_snapshotIdSlot));
        }
        if (_indexIdSlot) {
            bob.appendNumber("indexIdSlot", static_cast<long long>(*_indexIdSlot));
        }
        if (_indexKeySlot) {
            bob.appendNumber("indexKeySlot", static_cast<long long>(*_indexKeySlot));
        }
        if (_indexKeyPatternSlot) {
            bob.appendNumber("indexKeyPatternSlot", static_cast<long long>(*_indexKeyPatternSlot));
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
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_snapshotIdSlot) {
        DebugPrinter::addIdentifier(ret, _snapshotIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexIdSlot) {
        DebugPrinter::addIdentifier(ret, _indexIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexKeySlot) {
        DebugPrinter::addIdentifier(ret, _indexKeySlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexKeyPatternSlot) {
        DebugPrinter::addIdentifier(ret, _indexKeyPatternSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
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
    DebugPrinter::addIdentifier(ret, _collUuid.toString());
    ret.emplace_back("`\"");

    ret.emplace_back(_forward ? "true" : "false");

    ret.emplace_back(_oplogTsAccessor ? "true" : "false");

    return ret;
}

ParallelScanStage::ParallelScanStage(CollectionUUID collectionUuid,
                                     boost::optional<value::SlotId> recordSlot,
                                     boost::optional<value::SlotId> recordIdSlot,
                                     boost::optional<value::SlotId> snapshotIdSlot,
                                     boost::optional<value::SlotId> indexIdSlot,
                                     boost::optional<value::SlotId> indexKeySlot,
                                     boost::optional<value::SlotId> indexKeyPatternSlot,
                                     std::vector<std::string> fields,
                                     value::SlotVector vars,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId nodeId,
                                     ScanCallbacks callbacks)
    : PlanStage("pscan"_sd, yieldPolicy, nodeId),
      _collUuid(collectionUuid),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdSlot(indexIdSlot),
      _indexKeySlot(indexKeySlot),
      _indexKeyPatternSlot(indexKeyPatternSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)),
      _scanCallbacks(std::move(callbacks)) {
    invariant(_fields.size() == _vars.size());

    _state = std::make_shared<ParallelState>();
}

ParallelScanStage::ParallelScanStage(const std::shared_ptr<ParallelState>& state,
                                     CollectionUUID collectionUuid,
                                     boost::optional<value::SlotId> recordSlot,
                                     boost::optional<value::SlotId> recordIdSlot,
                                     boost::optional<value::SlotId> snapshotIdSlot,
                                     boost::optional<value::SlotId> indexIdSlot,
                                     boost::optional<value::SlotId> indexKeySlot,
                                     boost::optional<value::SlotId> indexKeyPatternSlot,
                                     std::vector<std::string> fields,
                                     value::SlotVector vars,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId nodeId,
                                     ScanCallbacks callbacks)
    : PlanStage("pscan"_sd, yieldPolicy, nodeId),
      _collUuid(collectionUuid),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdSlot(indexIdSlot),
      _indexKeySlot(indexKeySlot),
      _indexKeyPatternSlot(indexKeyPatternSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)),
      _state(state),
      _scanCallbacks(std::move(callbacks)) {
    invariant(_fields.size() == _vars.size());
}

std::unique_ptr<PlanStage> ParallelScanStage::clone() const {
    return std::make_unique<ParallelScanStage>(_state,
                                               _collUuid,
                                               _recordSlot,
                                               _recordIdSlot,
                                               _snapshotIdSlot,
                                               _indexIdSlot,
                                               _indexKeySlot,
                                               _indexKeyPatternSlot,
                                               _fields,
                                               _vars,
                                               _yieldPolicy,
                                               _commonStats.nodeId,
                                               _scanCallbacks);
}

void ParallelScanStage::prepare(CompileCtx& ctx) {
    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    if (_recordIdSlot) {
        _recordIdAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        auto [it, inserted] =
            _fieldAccessors.emplace(_fields[idx], std::make_unique<value::OwnedValueAccessor>());
        uassert(4822816, str::stream() << "duplicate field: " << _fields[idx], inserted);
        auto [itRename, insertedRename] = _varAccessors.emplace(_vars[idx], it->second.get());
        uassert(4822817, str::stream() << "duplicate field: " << _vars[idx], insertedRename);
    }

    if (_snapshotIdSlot) {
        _snapshotIdAccessor = ctx.getAccessor(*_snapshotIdSlot);
    }

    if (_indexIdSlot) {
        _indexIdAccessor = ctx.getAccessor(*_indexIdSlot);
    }

    if (_indexKeySlot) {
        _indexKeyAccessor = ctx.getAccessor(*_indexKeySlot);
    }

    if (_indexKeyPatternSlot) {
        _indexKeyPatternAccessor = ctx.getAccessor(*_indexKeyPatternSlot);
    }

    tassert(5709601, "'_coll' should not be initialized prior to 'acquireCollection()'", !_coll);
    std::tie(_coll, _collName, _catalogEpoch) = acquireCollection(_opCtx, _collUuid);
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
    if (slotsAccessible()) {
        if (_recordAccessor) {
            _recordAccessor->makeOwned();
        }
        if (_recordIdAccessor) {
            _recordIdAccessor->makeOwned();
        }
        for (auto& [fieldName, accessor] : _fieldAccessors) {
            accessor->makeOwned();
        }
    }

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

    _coll = restoreCollection(_opCtx, _collName, _collUuid, _catalogEpoch);

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

    if (!_coll) {
        // we're being opened after 'close()'. we need to re-acquire '_coll' in this case and
        // make some validity checks (the collection has not been dropped, renamed, etc.).
        tassert(5071013, "ParallelScanStage is not open but have _cursor", !_cursor);
        _coll = restoreCollection(_opCtx, _collName, _collUuid, _catalogEpoch);
    }

    if (_coll) {
        {
            stdx::unique_lock lock(_state->mutex);
            if (_state->ranges.empty()) {
                auto ranges = _coll->getRecordStore()->numRecords(_opCtx) / 10240;
                if (ranges < 2) {
                    _state->ranges.emplace_back(Range{RecordId{}, RecordId{}});
                } else {
                    if (ranges > 1024) {
                        ranges = 1024;
                    }
                    auto randomCursor = _coll->getRecordStore()->getRandomCursor(_opCtx);
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

        _cursor = _coll->getCursor(_opCtx);
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

    // We are about to call next() on a storage cursor so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the next() call.
    disableSlotAccess();

    if (!_cursor) {
        return trackPlanState(PlanState::IS_EOF);
    }

    checkForInterrupt(_opCtx);

    boost::optional<Record> nextRecord;

    // Loop until we have a valid result or we return EOF.
    do {
        auto needRange = needsRange();
        nextRecord = needRange ? nextRange() : _cursor->next();
        if (!nextRecord) {
            if (_scanCallbacks.indexKeyCorruptionCheckCallback) {
                tassert(5113711,
                        "Index key corruption check can only performed when inspecting the first "
                        "recordId in a range",
                        needRange);
                _scanCallbacks.indexKeyCorruptionCheckCallback(_opCtx,
                                                               _snapshotIdAccessor,
                                                               _indexKeyAccessor,
                                                               _indexKeyPatternAccessor,
                                                               _range.begin,
                                                               _collName);
            }
            return trackPlanState(PlanState::IS_EOF);
        }

        if (!_range.end.isNull() && nextRecord->id == _range.end) {
            setNeedsRange();
            nextRecord = boost::none;
            continue;
        }

        // Return EOF if the index key is found to be inconsistent.
        if (_scanCallbacks.indexKeyConsistencyCheckCallBack &&
            !_scanCallbacks.indexKeyConsistencyCheckCallBack(
                _opCtx, _snapshotIdAccessor, _indexIdAccessor, _indexKeyAccessor, *nextRecord)) {
            return trackPlanState(PlanState::IS_EOF);
        }
    } while (!nextRecord);

    if (_recordAccessor) {
        _recordAccessor->reset(false,
                               value::TypeTags::bsonObject,
                               value::bitcastFrom<const char*>(nextRecord->data.data()));
    }

    if (_recordIdAccessor) {
        _recordIdAccessor->reset(false,
                                 value::TypeTags::RecordId,
                                 value::bitcastFrom<int64_t>(nextRecord->id.getLong()));
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
                auto [tag, val] = bson::convertFrom<true>(be, end, sv.size());

                it->second->reset(false, tag, val);

                if ((--fieldsToMatch) == 0) {
                    // No need to scan any further so bail out early.
                    break;
                }
            }

            be = bson::advance(be, sv.size());
        }
    }

    return trackPlanState(PlanState::ADVANCED);
}

void ParallelScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
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
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_snapshotIdSlot) {
        DebugPrinter::addIdentifier(ret, _snapshotIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexIdSlot) {
        DebugPrinter::addIdentifier(ret, _indexIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexKeySlot) {
        DebugPrinter::addIdentifier(ret, _indexKeySlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexKeyPatternSlot) {
        DebugPrinter::addIdentifier(ret, _indexKeyPatternSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
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
    DebugPrinter::addIdentifier(ret, _collUuid.toString());
    ret.emplace_back("`\"");

    return ret;
}
}  // namespace sbe
}  // namespace mongo
