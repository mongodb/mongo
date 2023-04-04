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

#include "mongo/config.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
ScanStage::ScanStage(UUID collectionUuid,
                     boost::optional<value::SlotId> recordSlot,
                     boost::optional<value::SlotId> recordIdSlot,
                     boost::optional<value::SlotId> snapshotIdSlot,
                     boost::optional<value::SlotId> indexIdentSlot,
                     boost::optional<value::SlotId> indexKeySlot,
                     boost::optional<value::SlotId> indexKeyPatternSlot,
                     boost::optional<value::SlotId> oplogTsSlot,
                     std::vector<std::string> fields,
                     value::SlotVector vars,
                     boost::optional<value::SlotId> seekKeySlot,
                     bool forward,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     ScanCallbacks scanCallbacks,
                     bool lowPriority,
                     bool useRandomCursor,
                     bool participateInTrialRunTracking)
    : PlanStage(
          seekKeySlot ? "seek"_sd : "scan"_sd, yieldPolicy, nodeId, participateInTrialRunTracking),
      _collUuid(collectionUuid),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdentSlot(indexIdentSlot),
      _indexKeySlot(indexKeySlot),
      _indexKeyPatternSlot(indexKeyPatternSlot),
      _oplogTsSlot(oplogTsSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)),
      _seekKeySlot(seekKeySlot),
      _forward(forward),
      _scanCallbacks(std::move(scanCallbacks)),
      _useRandomCursor(useRandomCursor),
      _lowPriority(lowPriority) {
    invariant(_fields.size() == _vars.size());
    invariant(!_seekKeySlot || _forward);
    tassert(5567202,
            "The '_oplogTsSlot' cannot be set without 'ts' field in '_fields'",
            !_oplogTsSlot ||
                (std::find(_fields.begin(), _fields.end(), repl::OpTime::kTimestampFieldName) !=
                 _fields.end()));
    // We cannot use a random cursor if we are seeking or requesting a reverse scan.
    invariant(!_useRandomCursor || (!_seekKeySlot && _forward));
    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        const char* str = _fields[idx].c_str();
        auto len = _fields[idx].size();
        _fieldsBloomFilter.insert(str, len);
    }
}

std::unique_ptr<PlanStage> ScanStage::clone() const {
    return std::make_unique<ScanStage>(_collUuid,
                                       _recordSlot,
                                       _recordIdSlot,
                                       _snapshotIdSlot,
                                       _indexIdentSlot,
                                       _indexKeySlot,
                                       _indexKeyPatternSlot,
                                       _oplogTsSlot,
                                       _fields,
                                       _vars,
                                       _seekKeySlot,
                                       _forward,
                                       _yieldPolicy,
                                       _commonStats.nodeId,
                                       _scanCallbacks,
                                       _lowPriority,
                                       _useRandomCursor,
                                       _participateInTrialRunTracking);
}

void ScanStage::prepare(CompileCtx& ctx) {
    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    if (_recordIdSlot) {
        _recordIdAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    _fieldAccessors.resize(_fields.size());
    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        auto accessorPtr = &_fieldAccessors[idx];

        auto [itRename, insertedRename] = _varAccessors.emplace(_vars[idx], accessorPtr);
        uassert(4822815, str::stream() << "duplicate field: " << _vars[idx], insertedRename);

        if (_oplogTsSlot && _fields[idx] == repl::OpTime::kTimestampFieldName) {
            _tsFieldAccessor = accessorPtr;
        }

        const size_t offset = computeFieldMaskOffset(_fields[idx].c_str(), _fields[idx].size());
        _maskOffsetToFieldAccessors[offset] =
            stdx::visit(OverloadedVisitor{
                            [&](stdx::monostate _) -> FieldAccessorVariant {
                                return std::make_pair(StringData{_fields[idx]}, accessorPtr);
                            },
                            [&](std::pair<StringData, value::OwnedValueAccessor*> pair)
                                -> FieldAccessorVariant {
                                StringMap<value::OwnedValueAccessor*> map;
                                map.emplace(pair.first, pair.second);
                                map.emplace(_fields[idx], accessorPtr);
                                return map;
                            },
                            [&](StringMap<value::OwnedValueAccessor*> map) -> FieldAccessorVariant {
                                map.emplace(_fields[idx], accessorPtr);
                                return std::move(map);
                            }},
                        std::move(_maskOffsetToFieldAccessors[offset]));
    }

    if (_seekKeySlot) {
        _seekKeyAccessor = ctx.getAccessor(*_seekKeySlot);
    }

    if (_snapshotIdSlot) {
        _snapshotIdAccessor = ctx.getAccessor(*_snapshotIdSlot);
    }

    if (_indexIdentSlot) {
        _indexIdentAccessor = ctx.getAccessor(*_indexIdentSlot);
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

void ScanStage::doSaveState(bool relinquishCursor) {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (slotsAccessible()) {
        if (_recordAccessor &&
            _recordAccessor->getViewOfValue().first != value::TypeTags::Nothing) {
            auto [tag, val] = _recordAccessor->getViewOfValue();
            tassert(5975900, "expected scan to produce bson", tag == value::TypeTags::bsonObject);

            auto* raw = value::bitcastTo<const char*>(val);
            const auto size = ConstDataView(raw).read<LittleEndian<uint32_t>>();
            _lastReturned.clear();
            _lastReturned.assign(raw, raw + size);
        }
    }
#endif

    if (relinquishCursor) {
        if (_recordAccessor) {
            prepareForYielding(*_recordAccessor, slotsAccessible());
        }
        if (_recordIdAccessor) {
            // TODO: SERVER-72054
            // RecordId are currently (incorrectly) accessed after EOF, therefore
            // we must treat them as always accessible ratther invalidate them when slots are
            // disabled. We should use slotsAccessible() instead of true, once the bug is fixed.
            prepareForYielding(*_recordIdAccessor, true);
        }
        for (auto& accessor : _fieldAccessors) {
            prepareForYielding(accessor, slotsAccessible());
        }
    }

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (!_recordAccessor || !slotsAccessible()) {
        _lastReturned.clear();
    }
#endif

    if (auto cursor = getActiveCursor(); cursor != nullptr && relinquishCursor) {
        cursor->save();
    }

    if (auto cursor = getActiveCursor()) {
        cursor->setSaveStorageCursorOnDetachFromOperationContext(!relinquishCursor);
    }

    _indexCatalogEntryMap.clear();
    _coll.reset();
}

void ScanStage::doRestoreState(bool relinquishCursor) {
    invariant(_opCtx);
    invariant(!_coll);

    // If this stage has not been prepared, then yield recovery is a no-op.
    if (!_collName) {
        return;
    }

    tassert(5777408, "Catalog epoch should be initialized", _catalogEpoch);
    _coll = restoreCollection(_opCtx, *_collName, _collUuid, *_catalogEpoch);

    if (auto cursor = getActiveCursor(); cursor != nullptr) {
        if (relinquishCursor) {
            const auto tolerateCappedCursorRepositioning = false;
            const bool couldRestore = cursor->restore(tolerateCappedCursorRepositioning);
            uassert(
                ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. ",
                couldRestore);
        } else if (_coll->isCapped()) {
            // We cannot check for capped position lost here, as it requires us to reposition the
            // cursor, which would free the underlying value and break the contract of
            // restoreState(fullSave=false). So we defer the capped collection position lost check
            // to the following getNext() call by setting this flag.
            //
            // The intention in this codepath is to retain a valid and positioned cursor across
            // query yields / getMore commands. However, it is safe to reposition the cursor in
            // getNext() and we must reset the cursor for capped collections in order to check for
            // CappedPositionLost errors.
            _needsToCheckCappedPositionLost = true;
        }
    }

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (_recordAccessor && !_lastReturned.empty()) {
        auto [tag, val] = _recordAccessor->getViewOfValue();
        tassert(5975901, "expected scan to produce bson", tag == value::TypeTags::bsonObject);

        auto* raw = value::bitcastTo<const char*>(val);
        const auto size = ConstDataView(raw).read<LittleEndian<uint32_t>>();

        tassert(5975902,
                "expected scan recordAccessor contents to remain the same after yield",
                size == _lastReturned.size());
        tassert(5975903,
                "expected scan recordAccessor contents to remain the same after yield",
                std::memcmp(&_lastReturned[0], raw, size) == 0);
    }
#endif
}

void ScanStage::doDetachFromOperationContext() {
    if (auto cursor = getActiveCursor()) {
        cursor->detachFromOperationContext();
    }
    _priority.reset();
}

void ScanStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_lowPriority && _open && opCtx->getClient()->isFromUserConnection() &&
        opCtx->lockState()->shouldWaitForTicket()) {
        _priority.emplace(opCtx->lockState(), AdmissionContext::Priority::kLow);
    }
    if (auto cursor = getActiveCursor()) {
        cursor->reattachToOperationContext(opCtx);
    }
}

void ScanStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

PlanStage::TrialRunTrackerAttachResultMask ScanStage::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    _tracker = tracker;
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToStreamingStage;
}

RecordCursor* ScanStage::getActiveCursor() const {
    return _useRandomCursor ? _randomCursor.get() : _cursor.get();
}

void ScanStage::initKey() {
    auto [tag, val] = _seekKeyAccessor->getViewOfValue();
    const auto msgTag = tag;
    tassert(7104002,
            str::stream() << "Seek key is wrong type: " << msgTag,
            tag == value::TypeTags::RecordId);

    _key = *value::getRecordIdView(val);
}

void ScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;

    dassert(_opCtx);

    // Fast-path for handling the case where 'reOpen' is true.
    if (MONGO_likely(reOpen)) {
        dassert(_open && _coll && getActiveCursor());

        if (_seekKeyAccessor) {
            initKey();
        } else if (!_useRandomCursor) {
            _cursor = _coll->getCursor(_opCtx, _forward);
        } else {
            _randomCursor = _coll->getRecordStore()->getRandomCursor(_opCtx);
        }

        _firstGetNext = true;
        return;
    }

    // If we reach here, 'reOpen' is false. That means this stage is either being opened for the
    // first time ever, or this stage is being opened for the first time after calling close().
    tassert(5071004, "first open to ScanStage but reOpen=true", !reOpen && !_open);
    tassert(5071005, "ScanStage is not open but has a cursor", !getActiveCursor());
    tassert(5777401, "Collection name should be initialized", _collName);
    tassert(5777402, "Catalog epoch should be initialized", _catalogEpoch);

    // We need to re-acquire '_coll' in this case and make some validity checks (the collection has
    // not been dropped, renamed, etc).
    _coll = restoreCollection(_opCtx, *_collName, _collUuid, *_catalogEpoch);

    tassert(5959701, "restoreCollection() unexpectedly returned null in ScanStage", _coll);

    if (_scanCallbacks.scanOpenCallback) {
        _scanCallbacks.scanOpenCallback(_opCtx, _coll);
    }

    if (_seekKeyAccessor) {
        initKey();
        _cursor = _coll->getCursor(_opCtx, _forward);
    } else if (!_useRandomCursor) {
        _cursor = _coll->getCursor(_opCtx, _forward);
    } else {
        _randomCursor = _coll->getRecordStore()->getRandomCursor(_opCtx);
    }

    _open = true;
    _firstGetNext = true;
}

value::OwnedValueAccessor* ScanStage::getFieldAccessor(StringData name, size_t offset) const {
    return stdx::visit(
        OverloadedVisitor{
            [](const stdx::monostate& _) -> value::OwnedValueAccessor* { return nullptr; },
            [&](const std::pair<StringData, value::OwnedValueAccessor*> pair) {
                return (pair.first == name) ? pair.second : nullptr;
            },
            [&](const StringMap<value::OwnedValueAccessor*>& map) {
                auto it = map.find(name);
                return it == map.end() ? nullptr : it->second;
            }},
        _maskOffsetToFieldAccessors[offset]);
}

PlanState ScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_lowPriority && !_priority && _opCtx->getClient()->isFromUserConnection() &&
        _opCtx->lockState()->shouldWaitForTicket()) {
        _priority.emplace(_opCtx->lockState(), AdmissionContext::Priority::kLow);
    }

    // We are about to call next() on a storage cursor so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the next() call.
    disableSlotAccess();

    // This call to checkForInterrupt() may result in a call to save() or restore() on the entire
    // PlanStage tree if a yield occurs. It's important that we call checkForInterrupt() before
    // checking '_needsToCheckCappedPositionLost' since a call to restoreState() may set
    // '_needsToCheckCappedPositionLost'.
    checkForInterrupt(_opCtx);

    if (_needsToCheckCappedPositionLost) {
        _cursor->save();
        if (!_cursor->restore(false /* do not tolerate capped position lost */)) {
            uasserted(ErrorCodes::CappedPositionLost,
                      "CollectionScan died due to position in capped collection being deleted. ");
        }

        _needsToCheckCappedPositionLost = false;
    }

    auto res = _firstGetNext && _seekKeyAccessor;
    auto nextRecord = _useRandomCursor ? _randomCursor->next()
                                       : (res ? _cursor->seekExact(_key) : _cursor->next());
    _firstGetNext = false;

    if (!nextRecord) {
        // Only check our index key for corruption during the first call to 'getNext' and while
        // seeking.
        if (_scanCallbacks.indexKeyCorruptionCheckCallback) {
            tassert(5113712,
                    "Index key corruption check can only be performed on the first call "
                    "to getNext() during a seek",
                    res);
            tassert(5777400, "Collection name should be initialized", _collName);
            _scanCallbacks.indexKeyCorruptionCheckCallback(_opCtx,
                                                           _snapshotIdAccessor,
                                                           _indexKeyAccessor,
                                                           _indexKeyPatternAccessor,
                                                           _key,
                                                           *_collName);
        }
        _priority.reset();
        return trackPlanState(PlanState::IS_EOF);
    }

    // Return EOF if the index key is found to be inconsistent.
    if (_scanCallbacks.indexKeyConsistencyCheckCallback &&
        !_scanCallbacks.indexKeyConsistencyCheckCallback(_opCtx,
                                                         _indexCatalogEntryMap,
                                                         _snapshotIdAccessor,
                                                         _indexIdentAccessor,
                                                         _indexKeyAccessor,
                                                         _coll,
                                                         *nextRecord)) {
        _priority.reset();
        return trackPlanState(PlanState::IS_EOF);
    }

    if (_recordAccessor) {
        _recordAccessor->reset(false,
                               value::TypeTags::bsonObject,
                               value::bitcastFrom<const char*>(nextRecord->data.data()));
    }

    if (_recordIdAccessor) {
        _recordId = std::move(nextRecord->id);
        _recordIdAccessor->reset(
            false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_recordId));
    }

    if (!_fieldAccessors.empty()) {
        auto rawBson = nextRecord->data.data();
        auto start = rawBson + 4;
        auto end = rawBson + ConstDataView(rawBson).read<LittleEndian<uint32_t>>();
        auto last = end - 1;

        if (_fieldAccessors.size() == 1) {
            // If we're only looking for 1 field, then it's more efficient to forgo the hashtable
            // and just use equality comparison.
            auto name = StringData{_fields[0]};
            auto [tag, val] = [start, last, end, name] {
                for (auto bsonElement = start; bsonElement != last;) {
                    auto field = bson::fieldNameAndLength(bsonElement);
                    if (field == name) {
                        return bson::convertFrom<true>(bsonElement, end, field.size());
                    }
                    bsonElement = bson::advance(bsonElement, field.size());
                }
                return std::make_pair(value::TypeTags::Nothing, value::Value{0});
            }();

            _fieldAccessors.front().reset(false, tag, val);
        } else {
            // If we're looking for 2 or more fields, it's more efficient to use the hashtable.
            for (auto& accessor : _fieldAccessors) {
                accessor.reset();
            }

            auto fieldsToMatch = _fieldAccessors.size();
            for (auto bsonElement = start; bsonElement != last;) {
                // Oftentimes _fieldAccessors hashtable only has a few entries, but the object we're
                // scanning could have dozens of fields. In this common scenario, most hashtable
                // lookups will "miss" (i.e. they won't find a matching entry in the hashtable). To
                // optimize for this, we put a very simple bloom filter (requiring only a few basic
                // machine instructions) in front of the hashtable. When we "miss" in the bloom
                // filter, we can quickly skip over a field without having to generate the hash for
                // the field.
                auto field = bson::fieldNameAndLength(bsonElement);
                const size_t offset = computeFieldMaskOffset(field.rawData(), field.size());
                if (!(_fieldsBloomFilter.maybeContainsHash(computeFieldMask(offset)))) {
                    bsonElement = bson::advance(bsonElement, field.size());
                    continue;
                }

                auto accessor = getFieldAccessor(field, offset);
                if (accessor != nullptr) {
                    auto [tag, val] = bson::convertFrom<true>(bsonElement, end, field.size());
                    accessor->reset(false, tag, val);
                    if ((--fieldsToMatch) == 0) {
                        // No need to scan any further so bail out early.
                        break;
                    }
                }
                bsonElement = bson::advance(bsonElement, field.size());
            }
        }

        if (_oplogTsAccessor) {
            // If _oplogTsAccessor is set, then we check if the document had a "ts" field, and if
            // so we write the value of "ts" into _oplogTsAccessor. The engine uses mechanism to
            // keep track of the most recent timestamp that has been observed when scanning the
            // oplog collection.
            tassert(7097200, "Expected _tsFieldAccessor to be defined", _tsFieldAccessor);
            auto [tag, val] = _tsFieldAccessor->getViewOfValue();
            if (tag != value::TypeTags::Nothing) {
                auto&& [copyTag, copyVal] = value::copyValue(tag, val);
                _oplogTsAccessor->reset(true, copyTag, copyVal);
            }
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
    _indexCatalogEntryMap.clear();
    _cursor.reset();
    _randomCursor.reset();
    _coll.reset();
    _priority.reset();
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
        if (_indexIdentSlot) {
            bob.appendNumber("indexIdentSlot", static_cast<long long>(*_indexIdentSlot));
        }
        if (_indexKeySlot) {
            bob.appendNumber("indexKeySlot", static_cast<long long>(*_indexKeySlot));
        }
        if (_indexKeyPatternSlot) {
            bob.appendNumber("indexKeyPatternSlot", static_cast<long long>(*_indexKeyPatternSlot));
        }

        bob.append("fields", _fields);
        bob.append("outputSlots", _vars.begin(), _vars.end());
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
        DebugPrinter::addIdentifier(ret, _seekKeySlot.value());
    }

    if (_recordSlot) {
        DebugPrinter::addIdentifier(ret, _recordSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_snapshotIdSlot) {
        DebugPrinter::addIdentifier(ret, _snapshotIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexIdentSlot) {
        DebugPrinter::addIdentifier(ret, _indexIdentSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexKeySlot) {
        DebugPrinter::addIdentifier(ret, _indexKeySlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexKeyPatternSlot) {
        DebugPrinter::addIdentifier(ret, _indexKeyPatternSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_useRandomCursor) {
        DebugPrinter::addKeyword(ret, "random");
    }

    if (_lowPriority) {
        DebugPrinter::addKeyword(ret, "lowPriority");
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

size_t ScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_fields);
    size += size_estimator::estimate(_vars);
    size += size_estimator::estimate(_specificStats);
    return size;
}

ParallelScanStage::ParallelScanStage(UUID collectionUuid,
                                     boost::optional<value::SlotId> recordSlot,
                                     boost::optional<value::SlotId> recordIdSlot,
                                     boost::optional<value::SlotId> snapshotIdSlot,
                                     boost::optional<value::SlotId> indexIdentSlot,
                                     boost::optional<value::SlotId> indexKeySlot,
                                     boost::optional<value::SlotId> indexKeyPatternSlot,
                                     std::vector<std::string> fields,
                                     value::SlotVector vars,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId nodeId,
                                     ScanCallbacks callbacks,
                                     bool participateInTrialRunTracking)
    : PlanStage("pscan"_sd, yieldPolicy, nodeId, participateInTrialRunTracking),
      _collUuid(collectionUuid),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdentSlot(indexIdentSlot),
      _indexKeySlot(indexKeySlot),
      _indexKeyPatternSlot(indexKeyPatternSlot),
      _fields(std::move(fields)),
      _vars(std::move(vars)),
      _scanCallbacks(std::move(callbacks)) {
    invariant(_fields.size() == _vars.size());

    _state = std::make_shared<ParallelState>();
}

ParallelScanStage::ParallelScanStage(const std::shared_ptr<ParallelState>& state,
                                     const UUID& collectionUuid,
                                     boost::optional<value::SlotId> recordSlot,
                                     boost::optional<value::SlotId> recordIdSlot,
                                     boost::optional<value::SlotId> snapshotIdSlot,
                                     boost::optional<value::SlotId> indexIdentSlot,
                                     boost::optional<value::SlotId> indexKeySlot,
                                     boost::optional<value::SlotId> indexKeyPatternSlot,
                                     std::vector<std::string> fields,
                                     value::SlotVector vars,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId nodeId,
                                     ScanCallbacks callbacks,
                                     bool participateInTrialRunTracking)
    : PlanStage("pscan"_sd, yieldPolicy, nodeId, participateInTrialRunTracking),
      _collUuid(collectionUuid),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdentSlot(indexIdentSlot),
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
                                               _indexIdentSlot,
                                               _indexKeySlot,
                                               _indexKeyPatternSlot,
                                               _fields,
                                               _vars,
                                               _yieldPolicy,
                                               _commonStats.nodeId,
                                               _scanCallbacks,
                                               _participateInTrialRunTracking);
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

    if (_indexIdentSlot) {
        _indexIdentAccessor = ctx.getAccessor(*_indexIdentSlot);
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

void ParallelScanStage::doSaveState(bool relinquishCursor) {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (slotsAccessible()) {
        if (_recordAccessor &&
            _recordAccessor->getViewOfValue().first != value::TypeTags::Nothing) {
            auto [tag, val] = _recordAccessor->getViewOfValue();
            tassert(5975904, "expected scan to produce bson", tag == value::TypeTags::bsonObject);

            auto* raw = value::bitcastTo<const char*>(val);
            const auto size = ConstDataView(raw).read<LittleEndian<uint32_t>>();
            _lastReturned.clear();
            _lastReturned.assign(raw, raw + size);
        }
    }
#endif

    if (_recordAccessor) {
        prepareForYielding(*_recordAccessor, slotsAccessible());
    }
    if (_recordIdAccessor) {
        // TODO: SERVER-72054
        // RecordId are currently (incorrectly) accessed after EOF, therefore
        // we must treat them as always accessible ratther invalidate them when slots are
        // disabled. We should use slotsAccessible() instead of true, once the bug is fixed.
        prepareForYielding(*_recordIdAccessor, true);
    }
    for (auto& [fieldName, accessor] : _fieldAccessors) {
        prepareForYielding(*accessor, slotsAccessible());
    }

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (!_recordAccessor || !slotsAccessible()) {
        _lastReturned.clear();
    }
#endif

    if (_cursor) {
        _cursor->save();
    }

    _indexCatalogEntryMap.clear();
    _coll.reset();
}

void ParallelScanStage::doRestoreState(bool relinquishCursor) {
    invariant(_opCtx);
    invariant(!_coll);

    // If this stage has not been prepared, then yield recovery is a no-op.
    if (!_collName) {
        return;
    }

    tassert(5777409, "Catalog epoch should be initialized", _catalogEpoch);
    _coll = restoreCollection(_opCtx, *_collName, _collUuid, *_catalogEpoch);

    if (_cursor && relinquishCursor) {
        const bool couldRestore = _cursor->restore();
        uassert(ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. ",
                couldRestore);
    }

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (_recordAccessor && !_lastReturned.empty()) {
        auto [tag, val] = _recordAccessor->getViewOfValue();
        tassert(5975905, "expected scan to produce bson", tag == value::TypeTags::bsonObject);

        auto* raw = value::bitcastTo<const char*>(val);
        const auto size = ConstDataView(raw).read<LittleEndian<uint32_t>>();

        tassert(5975906,
                "expected scan recordAccessor contents to remain the same after yield",
                size == _lastReturned.size());
        tassert(5975907,
                "expected scan recordAccessor contents to remain the same after yield",
                std::memcmp(&_lastReturned[0], raw, size) == 0);
    }
#endif
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
        tassert(5777403, "Collection name should be initialized", _collName);
        tassert(5777404, "Catalog epoch should be initialized", _catalogEpoch);
        _coll = restoreCollection(_opCtx, *_collName, _collUuid, *_catalogEpoch);
    }

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
                        rids.emplace(std::move(nextRecord->id));
                    }
                }
                RecordId lastid{};
                for (auto& id : rids) {
                    _state->ranges.emplace_back(Range{std::move(lastid), id});
                    lastid = std::move(id);
                }
                _state->ranges.emplace_back(Range{std::move(lastid), RecordId{}});
            }
        }
    }

    _cursor = _coll->getCursor(_opCtx);

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
                tassert(5777405, "Collection name should be initialized", _collName);
                _scanCallbacks.indexKeyCorruptionCheckCallback(_opCtx,
                                                               _snapshotIdAccessor,
                                                               _indexKeyAccessor,
                                                               _indexKeyPatternAccessor,
                                                               _range.begin,
                                                               *_collName);
            }
            return trackPlanState(PlanState::IS_EOF);
        }

        if (!_range.end.isNull() && nextRecord->id == _range.end) {
            setNeedsRange();
            nextRecord = boost::none;
            continue;
        }

        // Return EOF if the index key is found to be inconsistent.
        if (_scanCallbacks.indexKeyConsistencyCheckCallback &&
            !_scanCallbacks.indexKeyConsistencyCheckCallback(_opCtx,
                                                             _indexCatalogEntryMap,
                                                             _snapshotIdAccessor,
                                                             _indexIdentAccessor,
                                                             _indexKeyAccessor,
                                                             _coll,
                                                             *nextRecord)) {
            return trackPlanState(PlanState::IS_EOF);
        }
    } while (!nextRecord);

    if (_recordAccessor) {
        _recordAccessor->reset(false,
                               value::TypeTags::bsonObject,
                               value::bitcastFrom<const char*>(nextRecord->data.data()));
    }

    if (_recordIdAccessor) {
        _recordId = nextRecord->id;
        _recordIdAccessor->reset(
            false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_recordId));
    }


    if (!_fieldAccessors.empty()) {
        auto fieldsToMatch = _fieldAccessors.size();
        auto rawBson = nextRecord->data.data();
        auto be = rawBson + 4;
        auto end = rawBson + ConstDataView(rawBson).read<LittleEndian<uint32_t>>();
        for (auto& [name, accessor] : _fieldAccessors) {
            accessor->reset();
        }
        while (be != end - 1) {
            auto sv = bson::fieldNameAndLength(be);
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
    _indexCatalogEntryMap.clear();
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
        DebugPrinter::addIdentifier(ret, _recordSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_snapshotIdSlot) {
        DebugPrinter::addIdentifier(ret, _snapshotIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexIdentSlot) {
        DebugPrinter::addIdentifier(ret, _indexIdentSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexKeySlot) {
        DebugPrinter::addIdentifier(ret, _indexKeySlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_indexKeyPatternSlot) {
        DebugPrinter::addIdentifier(ret, _indexKeyPatternSlot.value());
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

size_t ParallelScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_fields);
    size += size_estimator::estimate(_vars);
    return size;
}

}  // namespace sbe
}  // namespace mongo
