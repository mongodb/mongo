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

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <cstring>
#include <set>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
/**
 * Regular constructor. Initializes static '_state' managed by a shared_ptr.
 */
ScanStage::ScanStage(UUID collUuid,
                     boost::optional<value::SlotId> recordSlot,
                     boost::optional<value::SlotId> recordIdSlot,
                     boost::optional<value::SlotId> snapshotIdSlot,
                     boost::optional<value::SlotId> indexIdentSlot,
                     boost::optional<value::SlotId> indexKeySlot,
                     boost::optional<value::SlotId> indexKeyPatternSlot,
                     boost::optional<value::SlotId> oplogTsSlot,
                     std::vector<std::string> scanFieldNames,
                     value::SlotVector scanFieldSlots,
                     boost::optional<value::SlotId> seekRecordIdSlot,
                     boost::optional<value::SlotId> minRecordIdSlot,
                     boost::optional<value::SlotId> maxRecordIdSlot,
                     bool forward,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     ScanCallbacks scanCallbacks,
                     // Optional arguments:
                     bool lowPriority,
                     bool useRandomCursor,
                     bool participateInTrialRunTracking,
                     bool includeScanStartRecordId,
                     bool includeScanEndRecordId)
    : PlanStage(seekRecordIdSlot ? "seek"_sd : "scan"_sd,
                yieldPolicy,
                nodeId,
                participateInTrialRunTracking),
      _state(std::make_shared<ScanStageState>(collUuid,
                                              recordSlot,
                                              recordIdSlot,
                                              snapshotIdSlot,
                                              indexIdentSlot,
                                              indexKeySlot,
                                              indexKeyPatternSlot,
                                              oplogTsSlot,
                                              scanFieldNames,
                                              scanFieldSlots,
                                              seekRecordIdSlot,
                                              minRecordIdSlot,
                                              maxRecordIdSlot,
                                              forward,
                                              scanCallbacks,
                                              useRandomCursor)),
      _includeScanStartRecordId(includeScanStartRecordId),
      _includeScanEndRecordId(includeScanEndRecordId),
      _lowPriority(lowPriority) {
    invariant(!seekRecordIdSlot || forward);
    // We cannot use a random cursor if we are seeking or requesting a reverse scan.
    invariant(!useRandomCursor || (!seekRecordIdSlot && forward));
}  // ScanStage regular constructor

/**
 * Constructor for clone(). Copies '_state' shared_ptr.
 */
ScanStage::ScanStage(const std::shared_ptr<ScanStageState>& state,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     bool lowPriority,
                     bool participateInTrialRunTracking,
                     bool includeScanStartRecordId,
                     bool includeScanEndRecordId)
    : PlanStage(state->seekRecordIdSlot ? "seek"_sd : "scan"_sd,
                yieldPolicy,
                nodeId,
                participateInTrialRunTracking),
      _state(state),
      _includeScanStartRecordId(includeScanStartRecordId),
      _includeScanEndRecordId(includeScanEndRecordId),
      _lowPriority(lowPriority) {}  // ScanStage constructor for clone()

std::unique_ptr<PlanStage> ScanStage::clone() const {
    return std::make_unique<ScanStage>(_state,
                                       _yieldPolicy,
                                       _commonStats.nodeId,
                                       _lowPriority,
                                       _participateInTrialRunTracking,
                                       _includeScanStartRecordId,
                                       _includeScanEndRecordId);
}

void ScanStage::prepare(CompileCtx& ctx) {
    const size_t numScanFields = _state->getNumScanFields();
    _scanFieldAccessors.resize(numScanFields);
    for (size_t idx = 0; idx < numScanFields; ++idx) {
        auto accessorPtr = &_scanFieldAccessors[idx];

        auto [itRename, insertedRename] =
            _scanFieldAccessorsMap.emplace(_state->scanFieldSlots[idx], accessorPtr);
        uassert(4822815,
                str::stream() << "duplicate field: " << _state->scanFieldSlots[idx],
                insertedRename);

        if (_state->oplogTsSlot &&
            _state->scanFieldNames[idx] == repl::OpTime::kTimestampFieldName) {
            // Oplog scans only: cache a pointer to the "ts" field accessor for fast access.
            _tsFieldAccessor = accessorPtr;
        }
    }

    if (_state->seekRecordIdSlot) {
        _seekRecordIdAccessor = ctx.getAccessor(*(_state->seekRecordIdSlot));
    }

    if (_state->minRecordIdSlot) {
        _minRecordIdAccessor = ctx.getAccessor(*(_state->minRecordIdSlot));
    }

    if (_state->maxRecordIdSlot) {
        _maxRecordIdAccessor = ctx.getAccessor(*(_state->maxRecordIdSlot));
    }

    if (_state->snapshotIdSlot) {
        _snapshotIdAccessor = ctx.getAccessor(*(_state->snapshotIdSlot));
    }

    if (_state->indexIdentSlot) {
        _indexIdentAccessor = ctx.getAccessor(*(_state->indexIdentSlot));
    }

    if (_state->indexKeySlot) {
        _indexKeyAccessor = ctx.getAccessor(*(_state->indexKeySlot));
    }

    if (_state->indexKeyPatternSlot) {
        _indexKeyPatternAccessor = ctx.getAccessor(*(_state->indexKeyPatternSlot));
    }

    if (_state->oplogTsSlot) {
        _oplogTsAccessor = ctx.getRuntimeEnvAccessor(*(_state->oplogTsSlot));
    }

    tassert(5709600, "'_coll' should not be initialized prior to 'acquireCollection()'", !_coll);
    _coll.acquireCollection(_opCtx, _state->collUuid);
}

value::SlotAccessor* ScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_state->recordSlot && *(_state->recordSlot) == slot) {
        return &_recordAccessor;
    }

    if (_state->recordIdSlot && *(_state->recordIdSlot) == slot) {
        return &_recordIdAccessor;
    }

    if (_state->oplogTsSlot && *(_state->oplogTsSlot) == slot) {
        return _oplogTsAccessor;
    }

    if (auto it = _scanFieldAccessorsMap.find(slot); it != _scanFieldAccessorsMap.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void ScanStage::doSaveState(bool relinquishCursor) {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (slotsAccessible()) {
        if (_state->recordSlot &&
            _recordAccessor.getViewOfValue().first != value::TypeTags::Nothing) {
            auto [tag, val] = _recordAccessor.getViewOfValue();
            tassert(5975900, "expected scan to produce bson", tag == value::TypeTags::bsonObject);

            auto* raw = value::bitcastTo<const char*>(val);
            const auto size = ConstDataView(raw).read<LittleEndian<uint32_t>>();
            _lastReturned.clear();
            _lastReturned.assign(raw, raw + size);
        }
    }
#endif

    if (relinquishCursor) {
        if (_state->recordSlot) {
            prepareForYielding(_recordAccessor, slotsAccessible());
        }
        if (_state->recordIdSlot) {
            // TODO: SERVER-72054
            // RecordId are currently (incorrectly) accessed after EOF, therefore we must treat them
            // as always accessible rather than invalidate them when slots are disabled. We should
            // use slotsAccessible() instead of true, once the bug is fixed.
            prepareForYielding(_recordIdAccessor, true);
        }
        for (auto& accessor : _scanFieldAccessors) {
            prepareForYielding(accessor, slotsAccessible());
        }
    }

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (!_state->recordSlot || !slotsAccessible()) {
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
    if (!_coll.getCollName()) {
        return;
    }

    _coll.restoreCollection(_opCtx, _state->collUuid);

    if (auto cursor = getActiveCursor(); cursor != nullptr) {
        if (relinquishCursor) {
            const auto tolerateCappedCursorRepositioning = false;
            const bool couldRestore = cursor->restore(tolerateCappedCursorRepositioning);
            uassert(
                ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. ",
                couldRestore);
        } else if (_coll.getPtr()->isCapped()) {
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
    if (_state->recordSlot && !_lastReturned.empty()) {
        auto [tag, val] = _recordAccessor.getViewOfValue();
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
    if (_lowPriority && _open && gDeprioritizeUnboundedUserCollectionScans.load() &&
        opCtx->getClient()->isFromUserConnection() &&
        shard_role_details::getLocker(opCtx)->shouldWaitForTicket(opCtx)) {
        _priority.emplace(opCtx, AdmissionContext::Priority::kLow);
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
    return _state->useRandomCursor ? _randomCursor.get() : _cursor.get();
}

void ScanStage::setSeekRecordId() {
    auto [tag, val] = _seekRecordIdAccessor->getViewOfValue();
    const auto msgTag = tag;
    tassert(7104002,
            str::stream() << "Seek key is wrong type: " << msgTag,
            tag == value::TypeTags::RecordId);

    _seekRecordId = *value::getRecordIdView(val);
}

void ScanStage::setMinRecordId() {
    auto [tag, val] = _minRecordIdAccessor->getViewOfValue();
    const auto msgTag = tag;
    tassert(7452101,
            str::stream() << "minRecordId is wrong type: " << msgTag,
            tag == value::TypeTags::RecordId);

    _minRecordId = *value::getRecordIdView(val);
}

void ScanStage::setMaxRecordId() {
    auto [tag, val] = _maxRecordIdAccessor->getViewOfValue();
    const auto msgTag = tag;
    tassert(7452102,
            str::stream() << "maxRecordId is wrong type: " << msgTag,
            tag == value::TypeTags::RecordId);

    _maxRecordId = *value::getRecordIdView(val);
}

void ScanStage::scanResetState(bool reOpen) {
    if (!_state->useRandomCursor) {
        // Reuse existing cursor if possible in the reOpen case (i.e. when we will do a seek).
        if (!reOpen ||
            (!_seekRecordIdAccessor &&
             (_state->forward ? !_minRecordIdAccessor : !_maxRecordIdAccessor))) {
            _cursor = _coll.getPtr()->getCursor(_opCtx, _state->forward);
        }
        if (_seekRecordIdAccessor) {
            setSeekRecordId();
        } else {
            if (_minRecordIdAccessor) {
                setMinRecordId();
            }
            if (_maxRecordIdAccessor) {
                setMaxRecordId();
            }
        }
    } else {
        _randomCursor = _coll.getPtr()->getRecordStore()->getRandomCursor(_opCtx);
    }

    _firstGetNext = true;
    _hasScanEndRecordId = _state->forward ? _maxRecordIdAccessor : _minRecordIdAccessor;
    _havePassedScanEndRecordId = false;
}

void ScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;

    dassert(_opCtx);

    // Fast-path for handling the case where 'reOpen' is true.
    if (MONGO_likely(reOpen)) {
        dassert(_open && _coll && getActiveCursor());
        scanResetState(reOpen);
        return;
    }

    // If we reach here, 'reOpen' is false. That means this stage is either being opened for the
    // first time ever, or this stage is being opened for the first time after calling close().
    tassert(5071004, "first open to ScanStage but reOpen=true", !reOpen && !_open);
    tassert(5071005, "ScanStage is not open but has a cursor", !getActiveCursor());

    // We need to re-acquire '_coll' in this case and make some validity checks (the collection has
    // not been dropped, renamed, etc).
    _coll.restoreCollection(_opCtx, _state->collUuid);

    tassert(5959701, "restoreCollection() unexpectedly returned null in ScanStage", _coll);

    if (_state->scanCallbacks.scanOpenCallback) {
        _state->scanCallbacks.scanOpenCallback(_opCtx, _coll.getPtr());
    }

    scanResetState(reOpen);
    _open = true;
}

PlanState ScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // A clustered collection scan may have an end bound we have already passed.
    if (_havePassedScanEndRecordId) {
        return trackPlanState(PlanState::IS_EOF);
    }

    if (_lowPriority && !_priority && gDeprioritizeUnboundedUserCollectionScans.load() &&
        _opCtx->getClient()->isFromUserConnection() &&
        shard_role_details::getLocker(_opCtx)->shouldWaitForTicket(_opCtx)) {
        _priority.emplace(_opCtx, AdmissionContext::Priority::kLow);
    }

    // We are about to call next() on a storage cursor so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the next() call.
    disableSlotAccess();

    // This call to checkForInterrupt() may result in a call to save() or restore() on the entire
    // PlanStage tree if a yield occurs. It's important that we call checkForInterrupt() before
    // checking '_needsToCheckCappedPositionLost' since a call to restoreState() may set
    // '_needsToCheckCappedPositionLost'.
    checkForInterruptAndYield(_opCtx);

    if (_needsToCheckCappedPositionLost) {
        _cursor->save();
        if (!_cursor->restore(false /* do not tolerate capped position lost */)) {
            uasserted(ErrorCodes::CappedPositionLost,
                      "CollectionScan died due to position in capped collection being deleted. ");
        }

        _needsToCheckCappedPositionLost = false;
    }

    // Optimized so the most common case has as short a codepath as possible. Info on bounds edge
    // enforcement:
    //   o '_seekRecordIdAccessor' existence means this is doing a single-record fetch or resuming a
    //     prior paused scan and must do seekExact() to that recordId. In the fetch case this is the
    //     record to be returned. In the resume case it is the last one returned before the pause,
    //     and if it no longer exists the scan will fail because it doesn't know where to resume
    //     from. If it is present, the code below expects us to leave the cursor on that record to
    //     do some checks, and there will be a FilterStage above the scan to filter out this record.
    //   o '_minRecordIdAccessor' and/or '_maxRecordIdAccessor' mean we are doing a bounded scan on
    //     a clustered collection, and we will do a seek() to the start bound on the first call.
    //     - If the bound(s) came in via an expression, we are to assume both bounds are inclusive.
    //       A FilterStage above this stage will exist to filter out any that are really exclusive.
    //     - If the bound(s) came in via the "min" and/or "max" keywords, this stage must enforce
    //       them directly as there may be no FilterStage above it. In this case the start bound is
    //       always inclusive, so the logic is unchanged, but the end bound is always exclusive, so
    //       we use '_includeScanEndRecordId' to indicate this for scan termination.
    bool doSeekExact = false;
    boost::optional<Record> nextRecord;
    if (!_state->useRandomCursor) {
        if (!_firstGetNext) {
            nextRecord = _cursor->next();
        } else {
            _firstGetNext = false;
            if (_seekRecordIdAccessor) {  // fetch or scan resume
                if (_seekRecordId.isNull()) {
                    // Attempting to resume from a null record ID gives a null '_seekRecordId'.
                    uasserted(ErrorCodes::KeyNotFound,
                              str::stream()
                                  << "Failed to resume collection scan: the recordId from "
                                     "which we are attempting to resume no longer exists in "
                                     "the collection: "
                                  << _seekRecordId);
                }
                doSeekExact = true;
                nextRecord = _cursor->seekExact(_seekRecordId);
            } else if (_minRecordIdAccessor && _state->forward) {
                // The range may be exclusive of the start record.
                // Find the first record equal to _minRecordId
                // or, if exclusive, the first record "after" it.
                nextRecord = _cursor->seek(_minRecordId,
                                           _includeScanStartRecordId
                                               ? SeekableRecordCursor::BoundInclusion::kInclude
                                               : SeekableRecordCursor::BoundInclusion::kExclude);
            } else if (_maxRecordIdAccessor && !_state->forward) {
                nextRecord = _cursor->seek(_maxRecordId,
                                           _includeScanStartRecordId
                                               ? SeekableRecordCursor::BoundInclusion::kInclude
                                               : SeekableRecordCursor::BoundInclusion::kExclude);
            } else {
                nextRecord = _cursor->next();
            }
        }
    } else {
        nextRecord = _randomCursor->next();
        // Performance optimization: random cursors don't care about '_firstGetNext' so we do not
        // need to set it to false here.
    }

    if (!nextRecord) {
        // Only check the index key for corruption if this getNext() call did seekExact(), as that
        // expects the '_seekRecordId' to be found, but it was not.
        if (doSeekExact && _state->scanCallbacks.indexKeyCorruptionCheckCallback) {
            tassert(5777400, "Collection name should be initialized", _coll.getCollName());
            _state->scanCallbacks.indexKeyCorruptionCheckCallback(_opCtx,
                                                                  _snapshotIdAccessor,
                                                                  _indexKeyAccessor,
                                                                  _indexKeyPatternAccessor,
                                                                  _seekRecordId,
                                                                  *_coll.getCollName());
        }

        // Indicate that the last recordId seen is null once EOF is hit.
        if (_state->recordIdSlot) {
            auto [tag, val] = sbe::value::makeCopyRecordId(RecordId());
            _recordIdAccessor.reset(true, tag, val);
        }
        _priority.reset();
        return trackPlanState(PlanState::IS_EOF);
    }

    // Return EOF if the index key is found to be inconsistent.
    if (_state->scanCallbacks.indexKeyConsistencyCheckCallback &&
        !_state->scanCallbacks.indexKeyConsistencyCheckCallback(_opCtx,
                                                                _indexCatalogEntryMap,
                                                                _snapshotIdAccessor,
                                                                _indexIdentAccessor,
                                                                _indexKeyAccessor,
                                                                _coll.getPtr(),
                                                                *nextRecord)) {
        _priority.reset();
        return trackPlanState(PlanState::IS_EOF);
    }

    if (_state->recordSlot) {
        _recordAccessor.reset(false,
                              value::TypeTags::bsonObject,
                              value::bitcastFrom<const char*>(nextRecord->data.data()));
    }

    if (_state->recordIdSlot) {
        _recordId = std::move(nextRecord->id);
        if (_hasScanEndRecordId) {
            if (_includeScanEndRecordId) {
                _havePassedScanEndRecordId =
                    _state->forward ? (_recordId > _maxRecordId) : (_recordId < _minRecordId);
            } else {
                _havePassedScanEndRecordId =
                    _state->forward ? (_recordId >= _maxRecordId) : (_recordId <= _minRecordId);
            }
        }
        if (_havePassedScanEndRecordId) {
            return trackPlanState(PlanState::IS_EOF);
        }
        _recordIdAccessor.reset(
            false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_recordId));
    }

    if (!_scanFieldAccessors.empty()) {
        auto rawBson = nextRecord->data.data();
        auto start = rawBson + 4;
        auto end = rawBson + ConstDataView(rawBson).read<LittleEndian<uint32_t>>();
        auto last = end - 1;

        if (_scanFieldAccessors.size() == 1) {
            // If we're only looking for 1 field, then it's more efficient to forgo the hashtable
            // and just use equality comparison.
            auto name = StringData{_state->scanFieldNames[0]};
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

            _scanFieldAccessors.front().reset(false, tag, val);
        } else {
            // If we're looking for 2 or more fields, it's more efficient to use the hashtable.
            for (auto& accessor : _scanFieldAccessors) {
                accessor.reset();
            }

            auto fieldsToMatch = _scanFieldAccessors.size();
            for (auto bsonElement = start; bsonElement != last;) {
                auto field = bson::fieldNameAndLength(bsonElement);
                auto accessor = getFieldAccessor(field);

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
            // Oplog scans only: if _oplogTsAccessor is set, the value of the "ts" field, if
            // it exists in the document, will be copied to this slot for use by the clustered scan
            // EOF filter above this stage and/or because the query asked for the latest "ts" value.
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
        if (_state->recordSlot) {
            bob.appendNumber("recordSlot", static_cast<long long>(*(_state->recordSlot)));
        }
        if (_state->recordIdSlot) {
            bob.appendNumber("recordIdSlot", static_cast<long long>(*(_state->recordIdSlot)));
        }
        if (_state->seekRecordIdSlot) {
            bob.appendNumber("seekRecordIdSlot",
                             static_cast<long long>(*(_state->seekRecordIdSlot)));
        }
        if (_state->minRecordIdSlot) {
            bob.appendNumber("minRecordIdSlot", static_cast<long long>(*(_state->minRecordIdSlot)));
        }
        if (_state->maxRecordIdSlot) {
            bob.appendNumber("maxRecordIdSlot", static_cast<long long>(*(_state->maxRecordIdSlot)));
        }
        if (_state->snapshotIdSlot) {
            bob.appendNumber("snapshotIdSlot", static_cast<long long>(*(_state->snapshotIdSlot)));
        }
        if (_state->indexIdentSlot) {
            bob.appendNumber("indexIdentSlot", static_cast<long long>(*(_state->indexIdentSlot)));
        }
        if (_state->indexKeySlot) {
            bob.appendNumber("indexKeySlot", static_cast<long long>(*(_state->indexKeySlot)));
        }
        if (_state->indexKeyPatternSlot) {
            bob.appendNumber("indexKeyPatternSlot",
                             static_cast<long long>(*(_state->indexKeyPatternSlot)));
        }

        bob.append("scanFieldNames", _state->scanFieldNames.getUnderlyingVector());
        bob.append("scanFieldSlots", _state->scanFieldSlots.begin(), _state->scanFieldSlots.end());
        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* ScanStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> ScanStage::debugPrint() const {
    std::vector<DebugPrinter::Block> ret = PlanStage::debugPrint();

    if (_state->seekRecordIdSlot) {
        DebugPrinter::addIdentifier(ret, _state->seekRecordIdSlot.value());
    }

    if (_state->recordSlot) {
        DebugPrinter::addIdentifier(ret, _state->recordSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_state->recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _state->recordIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_state->snapshotIdSlot) {
        DebugPrinter::addIdentifier(ret, _state->snapshotIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_state->indexIdentSlot) {
        DebugPrinter::addIdentifier(ret, _state->indexIdentSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_state->indexKeySlot) {
        DebugPrinter::addIdentifier(ret, _state->indexKeySlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_state->indexKeyPatternSlot) {
        DebugPrinter::addIdentifier(ret, _state->indexKeyPatternSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_state->minRecordIdSlot) {
        DebugPrinter::addIdentifier(ret, _state->minRecordIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_state->maxRecordIdSlot) {
        DebugPrinter::addIdentifier(ret, _state->maxRecordIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_state->useRandomCursor) {
        DebugPrinter::addKeyword(ret, "random");
    }

    if (_lowPriority) {
        DebugPrinter::addKeyword(ret, "lowPriority");
    }

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _state->scanFieldNames.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _state->scanFieldSlots[idx]);
        ret.emplace_back("=");
        DebugPrinter::addIdentifier(ret, _state->scanFieldNames[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _state->collUuid.toString());
    ret.emplace_back("`\"");

    ret.emplace_back(_state->forward ? "true" : "false");

    ret.emplace_back(_oplogTsAccessor ? "true" : "false");

    return ret;
}

size_t ScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_state->scanFieldNames.getUnderlyingVector());
    size += size_estimator::estimate(_state->scanFieldNames.getUnderlyingMap());
    size += size_estimator::estimate(_state->scanFieldSlots);
    size += size_estimator::estimate(_specificStats);
    return size;
}

ParallelScanStage::ParallelScanStage(UUID collUuid,
                                     boost::optional<value::SlotId> recordSlot,
                                     boost::optional<value::SlotId> recordIdSlot,
                                     boost::optional<value::SlotId> snapshotIdSlot,
                                     boost::optional<value::SlotId> indexIdentSlot,
                                     boost::optional<value::SlotId> indexKeySlot,
                                     boost::optional<value::SlotId> indexKeyPatternSlot,
                                     std::vector<std::string> scanFieldNames,
                                     value::SlotVector scanFieldSlots,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId nodeId,
                                     ScanCallbacks callbacks,
                                     bool participateInTrialRunTracking)
    : PlanStage("pscan"_sd, yieldPolicy, nodeId, participateInTrialRunTracking),
      _state(std::make_shared<ParallelState>()),
      _collUuid(collUuid),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdentSlot(indexIdentSlot),
      _indexKeySlot(indexKeySlot),
      _indexKeyPatternSlot(indexKeyPatternSlot),
      _scanFieldNames(scanFieldNames),
      _scanFieldSlots(scanFieldSlots),
      _scanCallbacks(callbacks) {
    invariant(_scanFieldNames.size() == _scanFieldSlots.size());
}

ParallelScanStage::ParallelScanStage(const std::shared_ptr<ParallelState>& state,
                                     UUID collUuid,
                                     boost::optional<value::SlotId> recordSlot,
                                     boost::optional<value::SlotId> recordIdSlot,
                                     boost::optional<value::SlotId> snapshotIdSlot,
                                     boost::optional<value::SlotId> indexIdentSlot,
                                     boost::optional<value::SlotId> indexKeySlot,
                                     boost::optional<value::SlotId> indexKeyPatternSlot,
                                     std::vector<std::string> scanFieldNames,
                                     value::SlotVector scanFieldSlots,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId nodeId,
                                     ScanCallbacks callbacks,
                                     bool participateInTrialRunTracking)
    : PlanStage("pscan"_sd, yieldPolicy, nodeId, participateInTrialRunTracking),
      _state(state),
      _collUuid(collUuid),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdentSlot(indexIdentSlot),
      _indexKeySlot(indexKeySlot),
      _indexKeyPatternSlot(indexKeyPatternSlot),
      _scanFieldNames(scanFieldNames),
      _scanFieldSlots(scanFieldSlots),
      _scanCallbacks(callbacks) {
    invariant(_scanFieldNames.size() == _scanFieldSlots.size());
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
                                               _scanFieldNames.getUnderlyingVector(),
                                               _scanFieldSlots,
                                               _yieldPolicy,
                                               _commonStats.nodeId,
                                               _scanCallbacks,
                                               _participateInTrialRunTracking);
}

void ParallelScanStage::prepare(CompileCtx& ctx) {
    _scanFieldAccessors.resize(_scanFieldNames.size());

    for (size_t idx = 0; idx < _scanFieldNames.size(); ++idx) {
        auto accessorPtr = &_scanFieldAccessors[idx];

        auto [itRename, insertedRename] =
            _scanFieldAccessorsMap.emplace(_scanFieldSlots[idx], accessorPtr);
        uassert(
            4822817, str::stream() << "duplicate field: " << _scanFieldSlots[idx], insertedRename);
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
    _coll.acquireCollection(_opCtx, _collUuid);
}

value::SlotAccessor* ParallelScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_recordSlot && *_recordSlot == slot) {
        return &_recordAccessor;
    }

    if (_recordIdSlot && *_recordIdSlot == slot) {
        return &_recordIdAccessor;
    }

    if (auto it = _scanFieldAccessorsMap.find(slot); it != _scanFieldAccessorsMap.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void ParallelScanStage::doSaveState(bool relinquishCursor) {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    _lastReturned.clear();
    if (slotsAccessible()) {
        if (_recordSlot && _recordAccessor.getViewOfValue().first != value::TypeTags::Nothing) {
            auto [tag, val] = _recordAccessor.getViewOfValue();
            tassert(5975904, "expected scan to produce bson", tag == value::TypeTags::bsonObject);

            auto* raw = value::bitcastTo<const char*>(val);
            const auto size = ConstDataView(raw).read<LittleEndian<uint32_t>>();
            _lastReturned.clear();
            _lastReturned.assign(raw, raw + size);
        }
    }
#endif

    if (_recordSlot) {
        prepareForYielding(_recordAccessor, slotsAccessible());
    }
    if (_recordIdSlot) {
        // TODO: SERVER-72054
        // RecordId are currently (incorrectly) accessed after EOF, therefore
        // we must treat them as always accessible ratther invalidate them when slots are
        // disabled. We should use slotsAccessible() instead of true, once the bug is fixed.
        prepareForYielding(_recordIdAccessor, true);
    }
    for (auto& accessor : _scanFieldAccessors) {
        prepareForYielding(accessor, slotsAccessible());
    }

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
    if (!_coll.getCollName()) {
        return;
    }

    _coll.restoreCollection(_opCtx, _collUuid);

    if (_cursor && relinquishCursor) {
        const bool couldRestore = _cursor->restore();
        uassert(ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. ",
                couldRestore);
    }

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (_recordSlot && !_lastReturned.empty()) {
        auto [tag, val] = _recordAccessor.getViewOfValue();
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
        _coll.restoreCollection(_opCtx, _collUuid);
    }

    {
        stdx::unique_lock lock(_state->mutex);
        if (_state->ranges.empty()) {
            auto ranges = _coll.getPtr()->getRecordStore()->numRecords(_opCtx) / 10240;
            if (ranges < 2) {
                _state->ranges.emplace_back(Range{RecordId{}, RecordId{}});
            } else {
                if (ranges > 1024) {
                    ranges = 1024;
                }
                auto randomCursor = _coll.getPtr()->getRecordStore()->getRandomCursor(_opCtx);
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
                    lastid = id;
                }
                _state->ranges.emplace_back(Range{std::move(lastid), RecordId{}});
            }
        }
    }

    _cursor = _coll.getPtr()->getCursor(_opCtx);

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

value::OwnedValueAccessor* ParallelScanStage::getFieldAccessor(StringData name) {
    if (size_t pos = _scanFieldNames.findPos(name); pos != StringListSet::npos) {
        return &_scanFieldAccessors[pos];
    }
    return nullptr;
}

PlanState ParallelScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call next() on a storage cursor so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the next() call.
    disableSlotAccess();

    if (!_cursor) {
        return trackPlanState(PlanState::IS_EOF);
    }

    checkForInterruptAndYield(_opCtx);

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
                tassert(5777405, "Collection name should be initialized", _coll.getCollName());
                _scanCallbacks.indexKeyCorruptionCheckCallback(_opCtx,
                                                               _snapshotIdAccessor,
                                                               _indexKeyAccessor,
                                                               _indexKeyPatternAccessor,
                                                               _range.begin,
                                                               *_coll.getCollName());
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
                                                             _coll.getPtr(),
                                                             *nextRecord)) {
            return trackPlanState(PlanState::IS_EOF);
        }
    } while (!nextRecord);

    if (_recordSlot) {
        _recordAccessor.reset(false,
                              value::TypeTags::bsonObject,
                              value::bitcastFrom<const char*>(nextRecord->data.data()));
    }

    if (_recordIdSlot) {
        _recordId = nextRecord->id;
        _recordIdAccessor.reset(
            false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_recordId));
    }


    if (!_scanFieldAccessors.empty()) {
        auto fieldsToMatch = _scanFieldAccessors.size();
        auto rawBson = nextRecord->data.data();
        auto be = rawBson + 4;
        auto end = rawBson + ConstDataView(rawBson).read<LittleEndian<uint32_t>>();
        for (auto& accessor : _scanFieldAccessors) {
            accessor.reset();
        }
        while (be != end - 1) {
            auto sv = bson::fieldNameAndLength(be);
            auto accessor = getFieldAccessor(sv);

            if (accessor != nullptr) {
                auto [tag, val] = bson::convertFrom<true>(be, end, sv.size());
                accessor->reset(false, tag, val);
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
    for (size_t idx = 0; idx < _scanFieldNames.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _scanFieldSlots[idx]);
        ret.emplace_back("=");
        DebugPrinter::addIdentifier(ret, _scanFieldNames[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _collUuid.toString());
    ret.emplace_back("`\"");

    return ret;
}

size_t ParallelScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_scanFieldNames.getUnderlyingVector());
    size += size_estimator::estimate(_scanFieldNames.getUnderlyingMap());
    size += size_estimator::estimate(_scanFieldSlots);
    return size;
}

}  // namespace sbe
}  // namespace mongo
