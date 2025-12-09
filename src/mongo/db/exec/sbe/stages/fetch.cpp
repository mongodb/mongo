/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/fetch.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

#include <cstdint>
#include <cstring>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace {
MONGO_FAIL_POINT_DEFINE(hangFetchGetNext);
}  // namespace

namespace mongo {
namespace sbe {
FetchStage::FetchStage(std::unique_ptr<PlanStage> child,
                       UUID collectionUuid,
                       DatabaseName dbName,
                       std::shared_ptr<FetchStageState> state,
                       PlanYieldPolicy* yieldPolicy,
                       PlanNodeId nodeId,
                       bool participateInTrialRunTracking)
    : PlanStage("fetch"_sd,
                yieldPolicy,
                nodeId,
                participateInTrialRunTracking,
                TrialRunTrackingType::TrackReads),
      _collectionUuid(collectionUuid),
      _dbName(dbName),
      _state(std::move(state)) {
    _children.emplace_back(std::move(child));
}

std::unique_ptr<PlanStage> FetchStage::clone() const {
    return std::make_unique<FetchStage>(_children[0]->clone(),
                                        _collectionUuid,
                                        _dbName,
                                        _state,
                                        _yieldPolicy,
                                        _commonStats.nodeId,
                                        participateInTrialRunTracking());
}

void FetchStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
    _seekRecordIdAccessor = _children[0]->getAccessor(ctx, _state->seekSlot);

    if (auto slot = _state->inSnapshotIdSlot) {
        _snapshotIdAccessor = _children[0]->getAccessor(ctx, *slot);
    }

    if (auto slot = _state->inIndexIdentSlot) {
        _indexIdentAccessor = _children[0]->getAccessor(ctx, *slot);
    }

    if (auto slot = _state->inIndexKeySlot) {
        _indexKeyAccessor = _children[0]->getAccessor(ctx, *slot);
    }

    if (auto slot = _state->inIndexKeyPatternSlot) {
        _indexKeyPatternAccessor = _children[0]->getAccessor(ctx, *slot);
    }

    const size_t numScanFields = _state->scanFieldNames.size();
    _scanFieldAccessors.resize(numScanFields);
    _scanFieldAccessorsMap.reserve(numScanFields);
    for (size_t idx = 0; idx < numScanFields; ++idx) {
        auto accessorPtr = &_scanFieldAccessors[idx];
        auto [itRename, insertedRename] =
            _scanFieldAccessorsMap.emplace(_state->scanFieldSlots[idx], accessorPtr);
        tassert(10794902,
                str::stream() << "duplicate field: " << _state->scanFieldSlots[idx],
                insertedRename);
    }

    _coll.acquireCollection(_opCtx, _dbName, _collectionUuid);
}

value::SlotAccessor* FetchStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (slot == _state->resultSlot) {
        return &_recordAccessor;
    }
    if (slot == _state->recordIdSlot) {
        return &_recordIdAccessor;
    }

    if (auto it = _scanFieldAccessorsMap.find(slot); it != _scanFieldAccessorsMap.end()) {
        return it->second;
    }

    return _children[0]->getAccessor(ctx, slot);
}

void FetchStage::doSaveState() {
    if (_state->resultSlot) {
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

    if (_cursor) {
        _cursor->saveUnpositioned();
    }

    _indexCatalogEntryMap.clear();
    _coll.reset();
}

void FetchStage::doRestoreState() {
    tassert(10794904, "Expected opCtx to be non-null", _opCtx);

    if (!_coll.isAcquisition()) {
        // If this stage has not been prepared, then yield recovery is a no-op.
        if (!_coll.getCollName()) {
            return;
        }
        _coll.restoreCollection(_opCtx, _dbName, _collectionUuid);
    }

    if (_cursor) {
        const auto tolerateCappedCursorRepositioning = false;
        const bool couldRestore = _cursor->restore(*shard_role_details::getRecoveryUnit(_opCtx),
                                                   tolerateCappedCursorRepositioning);
        uassert(ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. ",
                couldRestore);
    }
}

void FetchStage::doDetachFromOperationContext() {
    if (_cursor) {
        _cursor->detachFromOperationContext();
    }
}

void FetchStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_cursor) {
        _cursor->reattachToOperationContext(opCtx);
    }
}

void FetchStage::doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) {
    _coll.setCollAcquisition(mca.getCollectionAcquisitionFromUuid(_collectionUuid));
}

void FetchStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    _commonStats.opens++;

    if (!_coll.isAcquisition()) {
        // We need to re-acquire '_coll' in this case and make some validity checks (the collection
        // has not been dropped, renamed, etc).
        _coll.restoreCollection(_opCtx, _dbName, _collectionUuid);
    }

    // Reuse existing cursor if possible.
    if (!reOpen) {
        _cursor = _coll.getPtr()->getCursor(_opCtx, true /* forward */);
    }
    _children[0]->open(reOpen);
    _recordIdAccessor.reset(
        false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_seekRid));

    if (_state->fetchCallbacks.scanOpenCallback) {
        _state->fetchCallbacks.scanOpenCallback(_opCtx, _coll.getPtr());
    }
}

PlanState FetchStage::getNext() {
    if (MONGO_unlikely(hangFetchGetNext.shouldFail())) {
        hangFetchGetNext.pauseWhileSet();
    }

    // TODO SERVER-113879 minimize callstack size in this function, as it will be a hot path.
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call next() on a storage cursor so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the next() call.
    disableSlotAccess();

    boost::optional<Record> record;

    // TODO SERVER-113879 investigate whether callbacks can use a named function instead, to
    // guarantee inlining.
    auto checkRecordConsistency = [&]() -> bool {
        const auto callback = _state->fetchCallbacks.indexKeyConsistencyCheckCallback;
        if (!callback) {
            return true;
        }
        return callback(_opCtx,
                        _indexCatalogEntryMap,
                        _snapshotIdAccessor,
                        _indexIdentAccessor,
                        _indexKeyAccessor,
                        _coll.getPtr(),
                        *record);
    };

    // For as long as we haven't found a valid record, the record fails the consistency check,
    // try to seek the record id.
    do {
        auto state = _children[0]->getNext();
        if (state == PlanState::IS_EOF) {
            return trackPlanState(PlanState::IS_EOF);
        }

        auto [seekTag, seekVal] = _seekRecordIdAccessor->getViewOfValue();
        // TODO SERVER-113879 investigate assembly output of tassert to make sure it compiles
        // cleanly and doesn't disrupt hot path.
        tassert(10794903,
                str::stream() << "Seek key is wrong type: " << seekTag,
                seekTag == value::TypeTags::RecordId);

        _seekRid = *value::getRecordIdView(seekVal);
        tassert(11430201, "Cursor must not be null", _cursor);
        record = _cursor->seekExact(_seekRid);
        if (!record) {
            if (_state->fetchCallbacks.indexKeyCorruptionCheckCallback) {
                tassert(10794901, "Collection name should be initialized", _coll.getCollName());
                _state->fetchCallbacks.indexKeyCorruptionCheckCallback(_opCtx,
                                                                       _snapshotIdAccessor,
                                                                       _indexKeyAccessor,
                                                                       _indexKeyPatternAccessor,
                                                                       _seekRid,
                                                                       *_coll.getCollName());
            }
        }
    } while (!record || !checkRecordConsistency());

    _recordAccessor.reset(
        false, value::TypeTags::bsonObject, value::bitcastFrom<const char*>(record->data.data()));
    if (!_scanFieldAccessors.empty()) {
        placeFieldsFromRecordInAccessors(*record, _state->scanFieldNames, _scanFieldAccessors);
    }

    ++_specificStats.numReads;
    trackRead();
    return trackPlanState(PlanState::ADVANCED);
}

void FetchStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();
    _children[0]->close();
    _cursor.reset();
    _coll.reset();
}

std::unique_ptr<PlanStageStats> FetchStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    ret->specific = std::make_unique<FetchStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("numReads", static_cast<long long>(_specificStats.numReads));
        ret->debugInfo = bob.obj();
    }

    return ret;
}

const SpecificStats* FetchStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> FetchStage::debugPrint(
    const DebugPrintInfo& debugPrintInfo) const {
    auto ret = PlanStage::debugPrint(debugPrintInfo);
    DebugPrinter::addIdentifier(ret, _state->seekSlot);
    ret.emplace_back("=");
    DebugPrinter::addKeyword(ret, "seek");
    ret.emplace_back(DebugPrinter::Block("`,"));

    DebugPrinter::addIdentifier(ret, _state->resultSlot);
    ret.emplace_back("=");
    DebugPrinter::addKeyword(ret, "result");
    ret.emplace_back(DebugPrinter::Block("`,"));

    DebugPrinter::addIdentifier(ret, _state->recordIdSlot);
    ret.emplace_back("=");
    DebugPrinter::addKeyword(ret, "recordId");
    ret.emplace_back(DebugPrinter::Block("`,"));

    if (_state->inSnapshotIdSlot) {
        DebugPrinter::addIdentifier(ret, _state->inSnapshotIdSlot.value());
        ret.emplace_back("=");
        DebugPrinter::addKeyword(ret, "inSnapshotId");
        ret.emplace_back(DebugPrinter::Block("`,"));
    }

    if (_state->inIndexIdentSlot) {
        DebugPrinter::addIdentifier(ret, _state->inIndexIdentSlot.value());
        ret.emplace_back("=");
        DebugPrinter::addKeyword(ret, "inIndexIdent");
        ret.emplace_back(DebugPrinter::Block("`,"));
    }

    if (_state->inIndexKeySlot) {
        DebugPrinter::addIdentifier(ret, _state->inIndexKeySlot.value());
        ret.emplace_back("=");
        DebugPrinter::addKeyword(ret, "inIndexKey");
        ret.emplace_back(DebugPrinter::Block("`,"));
    }

    if (_state->inIndexKeyPatternSlot) {
        DebugPrinter::addIdentifier(ret, _state->inIndexKeyPatternSlot.value());
        ret.emplace_back("=");
        DebugPrinter::addKeyword(ret, "inIndexKeyPattern");
        // No trailing commma.
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
    DebugPrinter::addIdentifier(ret, _collectionUuid.toString());
    ret.emplace_back("`\"");


    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
    return ret;
}

size_t FetchStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_state->scanFieldNames);
    size += size_estimator::estimate(_state->scanFieldSlots);
    size += size_estimator::estimate(_specificStats);
    return size;
}
}  // namespace sbe
}  // namespace mongo
