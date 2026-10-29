// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/unordered_fetch.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/scan_helpers.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <utility>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

UnorderedFetchStage::UnorderedFetchStage(std::unique_ptr<PlanStage> child,
                                         UUID collectionUuid,
                                         DatabaseName dbName,
                                         value::SlotId seekSlot,
                                         value::SlotId resultSlot,
                                         value::SlotId recordIdSlot,
                                         std::vector<std::string> scanFieldNames,
                                         value::SlotVector scanFieldSlots,
                                         size_t maxRecordIdBatchCount,
                                         PlanYieldPolicySBE* yieldPolicy,
                                         PlanNodeId nodeId,
                                         bool participateInTrialRunTracking)
    : PlanStage("unordered_fetch"sv,
                yieldPolicy,
                nodeId,
                participateInTrialRunTracking,
                TrialRunTrackingType::TrackReads),
      _collectionUuid(collectionUuid),
      _dbName(std::move(dbName)),
      _seekSlot(seekSlot),
      _resultSlot(resultSlot),
      _recordIdSlot(recordIdSlot),
      _scanFieldNames(std::move(scanFieldNames)),
      _scanFieldSlots(std::move(scanFieldSlots)),
      _maxRecordIdBatchCount(maxRecordIdBatchCount) {
    tassert(13624807,
            "Unordered fetch maxRecordIdBatchCount must be positive",
            _maxRecordIdBatchCount > 0);
    tassert(13624808,
            "Unordered fetch field names and slots must have matching sizes",
            _scanFieldNames.size() == _scanFieldSlots.size());
    tassert(13624809,
            "Unordered fetch result and RecordId slots must be distinct",
            _resultSlot != _recordIdSlot);
    _children.emplace_back(std::move(child));
}

std::unique_ptr<PlanStage> UnorderedFetchStage::clone() const {
    return std::make_unique<UnorderedFetchStage>(
        _children[0]->clone(),
        _collectionUuid,
        _dbName,
        _seekSlot,
        _resultSlot,
        _recordIdSlot,
        std::vector<std::string>(_scanFieldNames.begin(), _scanFieldNames.end()),
        _scanFieldSlots,
        _maxRecordIdBatchCount,
        _yieldPolicy,
        _commonStats.nodeId,
        participateInTrialRunTracking());
}

void UnorderedFetchStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
    _seekAccessor = _children[0]->getAccessor(ctx, _seekSlot);
    _scanFieldAccessors.resize(_scanFieldNames.size());
    _scanFieldAccessorsMap.reserve(_scanFieldNames.size());

    for (size_t i = 0; i < _scanFieldNames.size(); ++i) {
        const auto slot = _scanFieldSlots[i];
        auto [it, inserted] = _scanFieldAccessorsMap.emplace(slot, &_scanFieldAccessors[i]);
        tassert(13624810,
                "Unordered fetch output slots must be distinct",
                inserted && slot != _resultSlot && slot != _recordIdSlot);
    }

    tassert(13624811, "MultipleCollectionAccessor must be set on CompileCtx", ctx.mca);
    _coll = ctx.mca->getCollectionAcquisitionFromUuid(_collectionUuid);
}

value::SlotAccessor* UnorderedFetchStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (slot == _resultSlot) {
        return &_recordAccessor;
    }

    if (slot == _recordIdSlot) {
        return &_recordIdAccessor;
    }

    if (auto it = _scanFieldAccessorsMap.find(slot); it != _scanFieldAccessorsMap.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void UnorderedFetchStage::resetBufferedState() {
    _recordIdBatch.clear();
    _numIdsConsumedFromBatch = 0;

    _pendingRecordIds.reset();
    _numPendingIdsConsumed = 0;

    _inputIdsExhausted = false;

    _recordAccessor.reset();
    _recordIdAccessor.reset();

    for (auto& accessor : _scanFieldAccessors) {
        accessor.reset();
    }
}

void UnorderedFetchStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    ++_commonStats.opens;

    tassert(13624812, "Expected collection to be an acquisition", _coll.has_value());

    resetBufferedState();
    _recordIdBatch.reserve(_maxRecordIdBatchCount);

    if (!reOpen) {
        _cursor = _coll->getCollectionPtr()->getCursor(_opCtx, true /* forward */);
    }

    tassert(13624813, "Unordered fetch cursor must be initialized", _cursor);

    _children[0]->open(reOpen);

    _recordIdAccessor.reset(
        value::TagValueView{value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_seekRid)});
}

void UnorderedFetchStage::fillBatch() {
    _recordIdBatch.clear();
    _numIdsConsumedFromBatch = 0;

    if (_inputIdsExhausted) {
        return;
    }

    while (true) {
        checkForInterruptNoYield(_opCtx);

        if (_pendingRecordIds && _numPendingIdsConsumed < _pendingRecordIds->getVector().size()) {
            const auto& ids = _pendingRecordIds->getVector();
            const auto count = std::min(_maxRecordIdBatchCount - _recordIdBatch.size(),
                                        ids.size() - _numPendingIdsConsumed);
            _recordIdBatch.insert(_recordIdBatch.end(),
                                  ids.begin() + _numPendingIdsConsumed,
                                  ids.begin() + _numPendingIdsConsumed + count);
            _numPendingIdsConsumed += count;
        }

        // Stop once we've filled the batch.
        if (_recordIdBatch.size() >= _maxRecordIdBatchCount) {
            break;
        }

        // Read one or more record ids from the child stage.
        if (_children[0]->getNext() == PlanState::IS_EOF) {
            _inputIdsExhausted = true;
            break;
        }

        auto [tagRecordIds, valRecordIds] = _seekAccessor->getViewOfValue();
        if (tagRecordIds == value::TypeTags::valueBlock) {
            // Take ownership of the value block from the seek accessor to ensure it remains valid.
            _pendingRecordIds.reset(
                value::getValueBlock(_seekAccessor->copyOrMoveValue().releaseToRaw().second)
                    ->as<value::Int64RecordIdBlock>());
            _numPendingIdsConsumed = 0;
            tassert(13624814,
                    "Unordered fetch input must be an Int64RecordIdBlock",
                    _pendingRecordIds != nullptr);
        } else if (tagRecordIds == value::TypeTags::RecordId) {
            // Single record id provided directly.
            auto recordId = value::getRecordIdView(valRecordIds);
            tassert(
                13624815, "Unordered fetch input must be an integer RecordId", recordId->isLong());
            _recordIdBatch.push_back(recordId->getLong());
        } else {
            tasserted(13624816,
                      "Unordered fetch input must be either a record id or block of record ids");
        }
    }

    std::sort(_recordIdBatch.begin(), _recordIdBatch.end());

    if (!_recordIdBatch.empty()) {
        ++_batches;
    }
    _rids += _recordIdBatch.size();
}

PlanState UnorderedFetchStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    disableSlotAccess();

    while (true) {
        if (_numIdsConsumedFromBatch >= _recordIdBatch.size()) {
            fillBatch();

            if (_recordIdBatch.empty()) {
                return trackPlanState(PlanState::IS_EOF);
            }
        }

        _seekRid = RecordId(_recordIdBatch[_numIdsConsumedFromBatch++]);
        auto record = _cursor->seekExact(_seekRid);
        if (!record) {
            // TODO SERVER-136754: Report lookup failure to the index key corruption check callback.
            continue;
        }

        _recordAccessor.reset(value::TagValueView{
            value::TypeTags::bsonObject, value::bitcastFrom<const char*>(record->data.data())});
        if (!_scanFieldAccessors.empty()) {
            placeFieldsFromRecordInAccessors(*record, _scanFieldNames, _scanFieldAccessors);
        }

        ++_specificStats.numReads;
        trackRead();

        return trackPlanState(PlanState::ADVANCED);
    }
}

void UnorderedFetchStage::doSaveState() {
    prepareForYielding(_recordAccessor, slotsAccessible());

    // Note: there is no need to prepare the '_recordIdAccessor' here, because it only ever
    // references an int64 record id that is always owned by the '_seekRid' member variable.

    for (auto& accessor : _scanFieldAccessors) {
        prepareForYielding(accessor, slotsAccessible());
    }

    if (_cursor) {
        // This stage only ever calls seekExact(), so the cursor position does not need to be
        // preserved.
        _cursor->saveUnpositioned();
    }
}

void UnorderedFetchStage::doRestoreState() {
    invariant(_opCtx);
    tassert(13624817, "Expected collection to be an acquisition", _coll.has_value());

    if (_cursor) {
        // The return value only indicates whether a positioned capped cursor can continue, which
        // is irrelevant because every read starts with a seekExact().
        _cursor->restore(*shard_role_details::getRecoveryUnit(_opCtx));
    }
}

void UnorderedFetchStage::doDetachFromOperationContext() {
    if (_cursor) {
        _cursor->detachFromOperationContext();
    }
}

void UnorderedFetchStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_cursor) {
        _cursor->reattachToOperationContext(opCtx);
    }
}

void UnorderedFetchStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();

    _children[0]->close();

    resetBufferedState();

    // Release the memory used by the record id batch.
    std::vector<int64_t>().swap(_recordIdBatch);

    _cursor.reset();
}

std::unique_ptr<PlanStageStats> UnorderedFetchStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    ret->specific = std::make_unique<FetchStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("numReads", static_cast<long long>(_specificStats.numReads));
        bob.appendNumber("maxRecordIdBatchCount", static_cast<long long>(_maxRecordIdBatchCount));
        bob.appendNumber("batches", static_cast<long long>(_batches));
        bob.appendNumber("rids", static_cast<long long>(_rids));
        ret->debugInfo = bob.obj();
    }

    return ret;
}

const SpecificStats* UnorderedFetchStage::getSpecificStats() const {
    return &_specificStats;
}

void UnorderedFetchStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                       DebugPrintInfo& debugPrintInfo) const {
    DebugPrinter::addIdentifier(ret, _seekSlot);
    DebugPrinter::addIdentifier(ret, _resultSlot);
    DebugPrinter::addIdentifier(ret, _recordIdSlot);
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t i = 0; i < _scanFieldNames.size(); ++i) {
        if (i) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _scanFieldSlots[i]);
        ret.emplace_back("=");
        DebugPrinter::addIdentifier(ret, _scanFieldNames[i]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));
    ret.emplace_back(std::to_string(_maxRecordIdBatchCount));
    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _collectionUuid.toString());
    ret.emplace_back("`\"");
    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t UnorderedFetchStage::estimateCompileTimeSize() const {
    return sizeof(*this) + size_estimator::estimate(_children) +
        size_estimator::estimate(_scanFieldNames) + size_estimator::estimate(_scanFieldSlots) +
        size_estimator::estimate(_specificStats);
}
}  // namespace mongo::sbe
