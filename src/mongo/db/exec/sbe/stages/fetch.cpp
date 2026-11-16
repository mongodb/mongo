// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/fetch.h"

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/index_key_checks.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

#include <cstring>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace {
MONGO_FAIL_POINT_DEFINE(hangFetchGetNext);
}  // namespace

namespace mongo {
namespace sbe {
using namespace std::literals::string_view_literals;
FetchStage::FetchStage(std::unique_ptr<PlanStage> child,
                       UUID collectionUuid,
                       DatabaseName dbName,
                       std::shared_ptr<FetchStageState> state,
                       PlanYieldPolicySBE* yieldPolicy,
                       PlanNodeId nodeId,
                       bool participateInTrialRunTracking)
    : PlanStage("fetch"sv,
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

    tassert(12546102, "MultipleCollectionAccessor must be set on CompileCtx", ctx.mca);
    _coll = ctx.mca->getCollectionAcquisitionFromUuid(_collectionUuid);
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
}

void FetchStage::doRestoreState() {
    tassert(10794904, "Expected opCtx to be non-null", _opCtx);

    tassert(12499904, "Expected collection to be an acquisition", _coll.has_value());

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

void FetchStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    _commonStats.opens++;

    tassert(12499905, "Expected collection to be an acquisition", _coll.has_value());

    // Reuse existing cursor if possible.
    if (!reOpen) {
        _cursor = _coll->getCollectionPtr()->getCursor(_opCtx, /*forward=*/true);
    }
    _children[0]->open(reOpen);
    _recordIdAccessor.reset(
        value::TagValueView{value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_seekRid)});
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

    // Look up index entries until we find one with a matching record in the collection that is
    // still consistent with the index key.
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

        if (_snapshotIdAccessor != nullptr) {
            auto [snapshotIdTag, snapshotIdVal] = _snapshotIdAccessor->getViewOfValue();
            const auto msgSnapshotIdTag = snapshotIdTag;
            tassert(5113701,
                    str::stream() << "SnapshotId is of wrong type: " << msgSnapshotIdTag,
                    snapshotIdTag == value::TypeTags::NumberInt64);
            auto snapshotId = value::bitcastTo<uint64_t>(snapshotIdVal);

            if (snapshotId ==
                shard_role_details::getRecoveryUnit(_opCtx)->getSnapshotId().toNumber()) {
                // We are fetching from the same snapshot that our index scan obtained its record id
                // from, so we expect to find the record unless we are observing the effects of a
                // prepared transaction conflict.
                if (!record) {
                    tassert(10794901, "Collection name should be initialized", _coll.has_value());
                    checkIndexKeyCorruption(_opCtx,
                                            _indexKeyAccessor,
                                            _indexKeyPatternAccessor,
                                            _seekRid,
                                            _coll->getCollectionPtr()->ns());
                }
            } else if (record) {
                // We found a record from a different snapshot than the one our index scan used. If
                // modifications to the document make it inconsistent with the index key, it's
                // possible that it no longer matches the predicate used to choose the index bounds,
                // so we skip it.
                if (!isIndexKeyConsistent(_opCtx,
                                          _indexCatalogEntryMap,
                                          _indexIdentAccessor,
                                          _indexKeyAccessor,
                                          _coll->getCollectionPtr(),
                                          *record)) {
                    record.reset();
                    continue;
                }
            } else {
                // We did not find a record, but we assume that the record existed in the snapshot
                // used for the index scan and does not exist in the current snapshot, which is not
                // an error.
            }
        }
    } while (!record);

    _recordAccessor.reset(value::TagValueView{
        value::TypeTags::bsonObject, value::bitcastFrom<const char*>(record->data.data())});
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

void FetchStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                              DebugPrintInfo& debugPrintInfo) const {
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
