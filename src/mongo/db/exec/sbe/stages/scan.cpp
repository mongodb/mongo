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

#include "mongo/db/exec/sbe/stages/scan.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

#include <cstdint>
#include <cstring>
#include <set>

#include <boost/optional/optional.hpp>

namespace {
MONGO_FAIL_POINT_DEFINE(hangScanGetNext);
}  // namespace

namespace mongo {
namespace sbe {
/**
 * Regular constructor. Initializes static '_state' managed by a shared_ptr.
 */
ScanStage::ScanStage(UUID collUuid,
                     DatabaseName dbName,
                     boost::optional<value::SlotId> recordSlot,
                     boost::optional<value::SlotId> recordIdSlot,
                     boost::optional<value::SlotId> snapshotIdSlot,
                     boost::optional<value::SlotId> indexIdentSlot,
                     boost::optional<value::SlotId> indexKeySlot,
                     boost::optional<value::SlotId> indexKeyPatternSlot,
                     std::vector<std::string> scanFieldNames,
                     value::SlotVector scanFieldSlots,
                     boost::optional<value::SlotId> minRecordIdSlot,
                     boost::optional<value::SlotId> maxRecordIdSlot,
                     bool forward,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     ScanCallbacks scanCallbacks,
                     // Optional arguments:
                     bool useRandomCursor,
                     bool participateInTrialRunTracking,
                     bool includeScanStartRecordId,
                     bool includeScanEndRecordId)
    : PlanStage("scan"_sd,
                yieldPolicy,
                nodeId,
                participateInTrialRunTracking,
                TrialRunTrackingType::TrackReads),
      _state(std::make_shared<ScanStageState>(collUuid,
                                              dbName,
                                              recordSlot,
                                              recordIdSlot,
                                              snapshotIdSlot,
                                              indexIdentSlot,
                                              indexKeySlot,
                                              indexKeyPatternSlot,
                                              scanFieldNames,
                                              scanFieldSlots,
                                              minRecordIdSlot,
                                              maxRecordIdSlot,
                                              forward,
                                              scanCallbacks,
                                              useRandomCursor)),
      _includeScanStartRecordId(includeScanStartRecordId),
      _includeScanEndRecordId(includeScanEndRecordId) {
    tassert(11094715,
            "Cannot use a random cursor if we are requesting a reverse scan",
            !useRandomCursor || forward);
}  // ScanStage regular constructor

/**
 * Constructor for clone(). Copies '_state' shared_ptr.
 */
ScanStage::ScanStage(const std::shared_ptr<ScanStageState>& state,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     bool participateInTrialRunTracking,
                     bool includeScanStartRecordId,
                     bool includeScanEndRecordId)
    : PlanStage("scan"_sd,
                yieldPolicy,
                nodeId,
                participateInTrialRunTracking,
                TrialRunTrackingType::TrackReads),
      _state(state),
      _includeScanStartRecordId(includeScanStartRecordId),
      _includeScanEndRecordId(includeScanEndRecordId) {}  // ScanStage constructor for clone()

std::unique_ptr<PlanStage> ScanStage::clone() const {
    return std::make_unique<ScanStage>(_state,
                                       _yieldPolicy,
                                       _commonStats.nodeId,
                                       participateInTrialRunTracking(),
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

    // No-op if using acquisition.
    _coll.acquireCollection(_opCtx, _state->dbName, _state->collUuid);
}

value::SlotAccessor* ScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_state->recordSlot && *(_state->recordSlot) == slot) {
        return &_recordAccessor;
    }

    if (_state->recordIdSlot && *(_state->recordIdSlot) == slot) {
        return &_recordIdAccessor;
    }

    if (auto it = _scanFieldAccessorsMap.find(slot); it != _scanFieldAccessorsMap.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void ScanStage::doSaveState() {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (slotsAccessible()) {
        if (_state->recordSlot &&
            _recordAccessor.getViewOfValue().tag != value::TypeTags::Nothing) {
            auto [tag, val] = _recordAccessor.getViewOfValue();
            tassert(5975900, "expected scan to produce bson", tag == value::TypeTags::bsonObject);

            auto* raw = value::bitcastTo<const char*>(val);
            const auto size = ConstDataView(raw).read<LittleEndian<uint32_t>>();
            _lastReturned.clear();
            _lastReturned.assign(raw, raw + size);
        }
    }
#endif

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

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    if (!_state->recordSlot || !slotsAccessible()) {
        _lastReturned.clear();
    }
#endif

    if (auto cursor = getActiveCursor(); cursor != nullptr) {
        cursor->save();
    }

    _indexCatalogEntryMap.clear();
    _coll.reset();
}

void ScanStage::doRestoreState() {
    invariant(_opCtx);

    if (!_coll.isAcquisition()) {
        // If this stage has not been prepared, then yield recovery is a no-op.
        if (!_coll.getCollName()) {
            return;
        }
        _coll.restoreCollection(_opCtx, _state->dbName, _state->collUuid);
    }

    if (auto cursor = getActiveCursor(); cursor != nullptr) {
        const auto tolerateCappedCursorRepositioning = false;
        const bool couldRestore = cursor->restore(*shard_role_details::getRecoveryUnit(_opCtx),
                                                  tolerateCappedCursorRepositioning);
        uassert(ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. ",
                couldRestore);
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
}

void ScanStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (auto cursor = getActiveCursor()) {
        cursor->reattachToOperationContext(opCtx);
    }
}

RecordCursor* ScanStage::getActiveCursor() const {
    return _state->useRandomCursor ? _randomCursor.get() : _cursor.get();
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
        if (!reOpen || (_state->forward ? !_minRecordIdAccessor : !_maxRecordIdAccessor)) {
            _cursor = _coll.getPtr()->getCursor(_opCtx, _state->forward);
        }
        if (_minRecordIdAccessor) {
            setMinRecordId();
        }
        if (_maxRecordIdAccessor) {
            setMaxRecordId();
        }
    } else {
        _randomCursor = _coll.getPtr()->getRecordStore()->getRandomCursor(
            _opCtx, *shard_role_details::getRecoveryUnit(_opCtx));
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
    if (!_coll.isAcquisition()) {
        // We need to re-acquire '_coll' in this case and make some validity checks (the collection
        // has not been dropped, renamed, etc).
        _coll.restoreCollection(_opCtx, _state->dbName, _state->collUuid);

        tassert(5959701, "restoreCollection() unexpectedly returned null in ScanStage", _coll);
    }

    if (_state->scanCallbacks.scanOpenCallback) {
        _state->scanCallbacks.scanOpenCallback(_opCtx, _coll.getPtr());
    }

    scanResetState(reOpen);
    _open = true;
}

void ScanStage::doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) {
    _coll.setCollAcquisition(mca.getCollectionAcquisitionFromUuid(_state->collUuid));
}

PlanState ScanStage::getNext() {
    if (MONGO_unlikely(hangScanGetNext.shouldFail())) {
        hangScanGetNext.pauseWhileSet();
    }

    auto optTimer(getOptTimer(_opCtx));

    // A clustered collection scan may have an end bound we have already passed.
    if (_havePassedScanEndRecordId) {
        return trackPlanState(PlanState::IS_EOF);
    }

    // We are about to call next() on a storage cursor so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the next() call.
    disableSlotAccess();

    // This call to checkForInterrupt() may result in a call to save() or restore() on the entire
    // PlanStage tree if a yield occurs.
    checkForInterruptAndYield(_opCtx);

    // Optimized so the most common case has as short a codepath as possible.
    // '_minRecordIdAccessor' and/or '_maxRecordIdAccessor' mean we are doing a bounded scan on
    // a clustered collection, and we will do a seek() to the start bound on the first call.
    // - If the bound(s) came in via an expression, we are to assume both bounds are inclusive.
    //   A FilterStage above this stage will exist to filter out any that are really exclusive.
    // - If the bound(s) came in via the "min" and/or "max" keywords, this stage must enforce
    //   them directly as there may be no FilterStage above it. In this case the start bound is
    //   always inclusive, so the logic is unchanged, but the end bound is always exclusive, so
    //   we use '_includeScanEndRecordId' to indicate this for scan termination.
    boost::optional<Record> nextRecord;
    if (!_state->useRandomCursor) {
        if (!_firstGetNext) {
            nextRecord = _cursor->next();
        } else {
            _firstGetNext = false;
            if (_minRecordIdAccessor && _state->forward) {
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
        // Indicate that the last recordId seen is null once EOF is hit.
        if (_state->recordIdSlot) {
            auto [tag, val] = sbe::value::makeCopyRecordId(RecordId());
            _recordIdAccessor.reset(true, tag, val);
        }
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
        placeFieldsFromRecordInAccessors(*nextRecord, _state->scanFieldNames, _scanFieldAccessors);
    }

    ++_specificStats.numReads;
    trackRead();
    return trackPlanState(PlanState::ADVANCED);
}

void ScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _indexCatalogEntryMap.clear();
    _cursor.reset();
    _randomCursor.reset();
    _coll.reset();
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
}  // namespace sbe
}  // namespace mongo
