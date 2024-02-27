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

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/str.h"

namespace mongo::sbe {
IndexScanStageBase::IndexScanStageBase(StringData stageType,
                                       UUID collUuid,
                                       StringData indexName,
                                       bool forward,
                                       boost::optional<value::SlotId> indexKeySlot,
                                       boost::optional<value::SlotId> recordIdSlot,
                                       boost::optional<value::SlotId> snapshotIdSlot,
                                       boost::optional<value::SlotId> indexIdentSlot,
                                       IndexKeysInclusionSet indexKeysToInclude,
                                       value::SlotVector vars,
                                       PlanYieldPolicy* yieldPolicy,
                                       PlanNodeId nodeId,
                                       bool lowPriority,
                                       bool participateInTrialRunTracking)
    : PlanStage(stageType, yieldPolicy, nodeId, participateInTrialRunTracking),
      _collUuid(collUuid),
      _indexName(indexName),
      _forward(forward),
      _indexKeySlot(indexKeySlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexIdentSlot(indexIdentSlot),
      _indexKeysToInclude(indexKeysToInclude),
      _vars(std::move(vars)),
      _lowPriority(lowPriority) {
    invariant(_indexKeysToInclude.count() == _vars.size());
}

void IndexScanStageBase::prepareImpl(CompileCtx& ctx) {
    _accessors.resize(_vars.size());
    for (size_t idx = 0; idx < _accessors.size(); ++idx) {
        auto [it, inserted] = _accessorMap.emplace(_vars[idx], &_accessors[idx]);
        uassert(4822821, str::stream() << "duplicate slot: " << _vars[idx], inserted);
    }

    tassert(5709602, "'_coll' should not be initialized prior to 'acquireCollection()'", !_coll);
    _coll.acquireCollection(_opCtx, _collUuid);

    auto indexCatalog = _coll.getPtr()->getIndexCatalog();
    auto indexDesc = indexCatalog->findIndexByName(_opCtx, _indexName);
    tassert(4938500,
            str::stream() << "could not find index named '" << _indexName << "' in collection '"
                          << _coll.getCollName()->toStringForErrorMsg() << "'",
            indexDesc);

    _uniqueIndex = indexDesc->unique();
    _entry = indexCatalog->getEntry(indexDesc);
    tassert(4938503,
            str::stream() << "expected IndexCatalogEntry for index named: " << _indexName,
            static_cast<bool>(_entry));

    _ordering = _entry->ordering();

    auto [identTag, identVal] = value::makeNewString(StringData(_entry->getIdent()));
    _indexIdentAccessor.reset(identTag, identVal);

    if (_indexIdentSlot) {
        _indexIdentViewAccessor.reset(identTag, identVal);
    } else {
        _indexIdentViewAccessor.reset();
    }

    if (_snapshotIdSlot) {
        _latestSnapshotId = shard_role_details::getRecoveryUnit(_opCtx)->getSnapshotId().toNumber();
    }
}

value::SlotAccessor* IndexScanStageBase::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_indexKeySlot && *_indexKeySlot == slot) {
        return &_recordAccessor;
    }

    if (_recordIdSlot && *_recordIdSlot == slot) {
        return &_recordIdAccessor;
    }

    if (_snapshotIdSlot && *_snapshotIdSlot == slot) {
        return &_snapshotIdAccessor;
    }

    if (_indexIdentSlot && *_indexIdentSlot == slot) {
        return &_indexIdentViewAccessor;
    }

    if (auto it = _accessorMap.find(slot); it != _accessorMap.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void IndexScanStageBase::doSaveState(bool relinquishCursor) {
    if (relinquishCursor) {
        if (_indexKeySlot) {
            prepareForYielding(_recordAccessor, slotsAccessible());
        }
        if (_recordIdSlot) {
            prepareForYielding(_recordIdAccessor, slotsAccessible());
        }
        for (auto& accessor : _accessors) {
            prepareForYielding(accessor, slotsAccessible());
        }

        if (_cursor) {
            _cursor->save();
        }
    }

    if (_cursor) {
        _cursor->setSaveStorageCursorOnDetachFromOperationContext(!relinquishCursor);
    }

    // Set the index entry to null, since accessing this pointer is illegal during yield.
    _entry = nullptr;

    _coll.reset();
}

void IndexScanStageBase::restoreCollectionAndIndex() {
    _coll.restoreCollection(_opCtx, _collUuid);

    auto [identTag, identVal] = _indexIdentAccessor.getViewOfValue();
    tassert(7566700, "Expected ident to be a string", value::isString(identTag));

    auto indexIdent = value::getStringView(identTag, identVal);
    auto desc = _coll.getPtr()->getIndexCatalog()->findIndexByIdent(_opCtx, indexIdent);
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: index '" << _indexName << "' dropped",
            desc);

    // Re-obtain the index entry pointer that was set to null during yield preparation. It is safe
    // to access the index entry when the query is active, as its validity is protected by at least
    // MODE_IS collection locks; or, in the case of lock-free reads, its lifetime is managed by the
    // CollectionCatalog stashed on the RecoveryUnit snapshot, which is kept alive until the query
    // yields.
    _entry = desc->getEntry();
}

void IndexScanStageBase::doRestoreState(bool relinquishCursor) {
    invariant(_opCtx);
    invariant(!_coll);

    // If this stage has not been prepared, then yield recovery is a no-op.
    if (!_coll.getCollName()) {
        return;
    }
    restoreCollectionAndIndex();

    if (_cursor && relinquishCursor) {
        _cursor->restore();
    }

    // Yield is the only time during plan execution that the snapshotId can change. As such, we
    // update it accordingly as part of yield recovery.
    if (_snapshotIdSlot) {
        _latestSnapshotId = shard_role_details::getRecoveryUnit(_opCtx)->getSnapshotId().toNumber();
    }
}

void IndexScanStageBase::doDetachFromOperationContext() {
    if (_cursor) {
        _cursor->detachFromOperationContext();
    }
    _priority.reset();
}

void IndexScanStageBase::doAttachToOperationContext(OperationContext* opCtx) {
    if (_lowPriority && _open && gDeprioritizeUnboundedUserIndexScans.load() &&
        _opCtx->getClient()->isFromUserConnection() &&
        shard_role_details::getLocker(_opCtx)->shouldWaitForTicket(_opCtx)) {
        _priority.emplace(opCtx, AdmissionContext::Priority::kLow);
    }
    if (_cursor) {
        _cursor->reattachToOperationContext(opCtx);
    }
}

void IndexScanStageBase::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

PlanStage::TrialRunTrackerAttachResultMask IndexScanStageBase::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    _tracker = tracker;
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToStreamingStage;
}

void IndexScanStageBase::openImpl(bool reOpen) {
    _commonStats.opens++;

    dassert(_opCtx);

    if (reOpen) {
        dassert(_open && _coll && _cursor);
        _scanState = ScanState::kNeedSeek;
        return;
    }

    tassert(5071009, "first open to IndexScanStageBase but reOpen=true", !_open);

    if (!_coll) {
        // We're being opened for the first time after 'close()', or we're being opened for the
        // first time ever. We need to re-acquire '_coll' in this case and make some validity
        // checks (the collection has not been dropped, renamed, etc).
        tassert(5071010, "IndexScanStageBase is not open but have _cursor", !_cursor);
        restoreCollectionAndIndex();
    }

    if (!_cursor) {
        _cursor = _entry->accessMethod()->asSortedData()->newCursor(_opCtx, _forward);
    }

    _open = true;
    _scanState = ScanState::kNeedSeek;
}

void IndexScanStageBase::trackRead() {
    ++_specificStats.numReads;
    if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumReads>(1)) {
        // If we're collecting execution stats during multi-planning and reached the end of the
        // trial period because we've performed enough physical reads, bail out from the trial run
        // by raising a special exception to signal a runtime planner that this candidate plan has
        // completed its trial run early. Note that a trial period is executed only once per a
        // PlanStage tree, and once completed never run again on the same tree.
        _tracker = nullptr;
        uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in ixscan");
    }
}

PlanState IndexScanStageBase::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_lowPriority && !_priority && gDeprioritizeUnboundedUserIndexScans.load() &&
        _opCtx->getClient()->isFromUserConnection() &&
        shard_role_details::getLocker(_opCtx)->shouldWaitForTicket(_opCtx)) {
        _priority.emplace(_opCtx, AdmissionContext::Priority::kLow);
    }

    // We are about to get next record from a storage cursor so do not bother saving our internal
    // state in case it yields as the state will be completely overwritten after the call.
    disableSlotAccess();

    checkForInterruptAndYield(_opCtx);

    do {
        switch (_scanState) {
            case ScanState::kNeedSeek:
                ++_specificStats.seeks;
                trackRead();
                // Seek for key and establish the cursor position.
                _nextRecord = seek();
                break;
            case ScanState::kScanning:
                trackRead();
                _nextRecord = _cursor->nextKeyString();
                break;
            case ScanState::kFinished:
                _priority.reset();
                return trackPlanState(PlanState::IS_EOF);
        }
    } while (!validateKey(_nextRecord));

    if (_indexKeySlot) {
        _recordAccessor.reset(false,
                              value::TypeTags::ksValue,
                              value::bitcastFrom<key_string::Value*>(&_nextRecord->keyString));
    }

    if (_recordIdSlot) {
        _recordIdAccessor.reset(
            false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_nextRecord->loc));
    }

    if (_snapshotIdSlot) {
        // Copy the latest snapshot ID into the 'snapshotId' slot.
        _snapshotIdAccessor.reset(value::TypeTags::NumberInt64,
                                  value::bitcastFrom<uint64_t>(_latestSnapshotId));
    }

    if (_accessors.size()) {
        _valuesBuffer.reset();
        readKeyStringValueIntoAccessors(
            _nextRecord->keyString, *_ordering, &_valuesBuffer, &_accessors, _indexKeysToInclude);
    }

    return trackPlanState(PlanState::ADVANCED);
}

void IndexScanStageBase::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();

    _cursor.reset();
    _coll.reset();
    _open = false;
}

std::unique_ptr<PlanStageStats> IndexScanStageBase::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<IndexScanStats>(_specificStats);

    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob;
        bob.append("indexName", _indexName);
        bob.appendNumber("keysExamined", static_cast<long long>(_specificStats.keysExamined));
        bob.appendNumber("seeks", static_cast<long long>(_specificStats.seeks));
        bob.appendNumber("numReads", static_cast<long long>(_specificStats.numReads));
        if (_indexKeySlot) {
            bob.appendNumber("indexKeySlot", static_cast<long long>(*_indexKeySlot));
        }
        if (_recordIdSlot) {
            bob.appendNumber("recordIdSlot", static_cast<long long>(*_recordIdSlot));
        }
        if (_snapshotIdSlot) {
            bob.appendNumber("snapshotIdSlot", static_cast<long long>(*_snapshotIdSlot));
        }
        if (_indexIdentSlot) {
            bob.appendNumber("indexIdentSlot", static_cast<long long>(*_indexIdentSlot));
        }
        bob.append("outputSlots", _vars.begin(), _vars.end());
        bob.append("indexKeysToInclude", _indexKeysToInclude.to_string());
        ret->debugInfo = bob.obj();
    }

    return ret;
}

const SpecificStats* IndexScanStageBase::getSpecificStats() const {
    return &_specificStats;
}

void IndexScanStageBase::debugPrintImpl(std::vector<DebugPrinter::Block>& blocks) const {
    if (_indexKeySlot) {
        DebugPrinter::addIdentifier(blocks, _indexKeySlot.value());
    } else {
        DebugPrinter::addIdentifier(blocks, DebugPrinter::kNoneKeyword);
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(blocks, _recordIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(blocks, DebugPrinter::kNoneKeyword);
    }

    if (_snapshotIdSlot) {
        DebugPrinter::addIdentifier(blocks, _snapshotIdSlot.value());
    } else {
        DebugPrinter::addIdentifier(blocks, DebugPrinter::kNoneKeyword);
    }

    if (_indexIdentSlot) {
        DebugPrinter::addIdentifier(blocks, _indexIdentSlot.value());
    } else {
        DebugPrinter::addIdentifier(blocks, DebugPrinter::kNoneKeyword);
    }

    if (_lowPriority) {
        DebugPrinter::addKeyword(blocks, "lowPriority");
    }

    blocks.emplace_back(DebugPrinter::Block("[`"));
    size_t varIndex = 0;
    for (size_t keyIndex = 0; keyIndex < _indexKeysToInclude.size(); ++keyIndex) {
        if (!_indexKeysToInclude[keyIndex]) {
            continue;
        }
        if (varIndex) {
            blocks.emplace_back(DebugPrinter::Block("`,"));
        }
        invariant(varIndex < _vars.size());
        DebugPrinter::addIdentifier(blocks, _vars[varIndex++]);
        blocks.emplace_back("=");
        blocks.emplace_back(std::to_string(keyIndex));
    }
    blocks.emplace_back(DebugPrinter::Block("`]"));

    blocks.emplace_back("@\"`");
    DebugPrinter::addIdentifier(blocks, _collUuid.toString());
    blocks.emplace_back("`\"");

    blocks.emplace_back("@\"`");
    DebugPrinter::addIdentifier(blocks, _indexName);
    blocks.emplace_back("`\"");

    blocks.emplace_back(_forward ? "true" : "false");
}

size_t IndexScanStageBase::estimateCompileTimeSizeImpl() const {
    size_t size = size_estimator::estimate(_vars);
    size += size_estimator::estimate(_indexName);
    size += size_estimator::estimate(_valuesBuffer);
    size += size_estimator::estimate(_specificStats);
    return size;
}

std::string IndexScanStageBase::getIndexName() const {
    return _indexName;
}

SimpleIndexScanStage::SimpleIndexScanStage(UUID collUuid,
                                           StringData indexName,
                                           bool forward,
                                           boost::optional<value::SlotId> indexKeySlot,
                                           boost::optional<value::SlotId> recordIdSlot,
                                           boost::optional<value::SlotId> snapshotIdSlot,
                                           boost::optional<value::SlotId> indexIdentSlot,
                                           IndexKeysInclusionSet indexKeysToInclude,
                                           value::SlotVector vars,
                                           std::unique_ptr<EExpression> seekKeyLow,
                                           std::unique_ptr<EExpression> seekKeyHigh,
                                           PlanYieldPolicy* yieldPolicy,
                                           PlanNodeId nodeId,
                                           bool lowPriority,
                                           bool participateInTrialRunTracking)
    : IndexScanStageBase(seekKeyLow ? "ixseek"_sd : "ixscan"_sd,
                         collUuid,
                         indexName,
                         forward,
                         indexKeySlot,
                         recordIdSlot,
                         snapshotIdSlot,
                         indexIdentSlot,
                         indexKeysToInclude,
                         std::move(vars),
                         yieldPolicy,
                         nodeId,
                         lowPriority,
                         participateInTrialRunTracking),
      _seekKeyLow(std::move(seekKeyLow)),
      _seekKeyHigh(std::move(seekKeyHigh)) {
    // The valid state is when both boundaries, or none is set, or only low key is set.
    invariant((_seekKeyLow && _seekKeyHigh) || (!_seekKeyLow && !_seekKeyHigh) ||
              (_seekKeyLow && !_seekKeyHigh));
}

std::unique_ptr<PlanStage> SimpleIndexScanStage::clone() const {
    return std::make_unique<SimpleIndexScanStage>(_collUuid,
                                                  _indexName,
                                                  _forward,
                                                  _indexKeySlot,
                                                  _recordIdSlot,
                                                  _snapshotIdSlot,
                                                  _indexIdentSlot,
                                                  _indexKeysToInclude,
                                                  _vars,
                                                  _seekKeyLow ? _seekKeyLow->clone() : nullptr,
                                                  _seekKeyHigh ? _seekKeyHigh->clone() : nullptr,
                                                  _yieldPolicy,
                                                  _commonStats.nodeId,
                                                  _lowPriority,
                                                  _participateInTrialRunTracking);
}

void SimpleIndexScanStage::prepare(CompileCtx& ctx) {
    IndexScanStageBase::prepareImpl(ctx);

    if (_seekKeyLow) {
        ctx.root = this;
        _seekKeyLowCode = _seekKeyLow->compile(ctx);
    }
    if (_seekKeyHigh) {
        ctx.root = this;
        _seekKeyHighCode = _seekKeyHigh->compile(ctx);
    }

    _seekKeyLowHolder.reset();
    _seekKeyHighHolder.reset();
}

void SimpleIndexScanStage::doSaveState(bool relinquishCursor) {
    // Seek points are external to the index scan and must be accessible no matter what as long
    // as the index scan is opened.
    if (_open && relinquishCursor) {
        if (_seekKeyLow) {
            prepareForYielding(_seekKeyLowHolder, true);
        }
        if (_seekKeyHigh) {
            prepareForYielding(_seekKeyHighHolder, true);
        }
    }

    IndexScanStageBase::doSaveState(relinquishCursor);
}

void SimpleIndexScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    IndexScanStageBase::openImpl(reOpen);

    if (_seekKeyLow && _seekKeyHigh) {
        auto [ownedLow, tagLow, valLow] = _bytecode.run(_seekKeyLowCode.get());
        const auto msgTagLow = tagLow;
        uassert(4822851,
                str::stream() << "seek key is wrong type: " << msgTagLow,
                tagLow == value::TypeTags::ksValue);
        _seekKeyLowHolder.reset(ownedLow, tagLow, valLow);

        auto [ownedHi, tagHi, valHi] = _bytecode.run(_seekKeyHighCode.get());
        const auto msgTagHi = tagHi;
        uassert(4822852,
                str::stream() << "seek key is wrong type: " << msgTagHi,
                tagHi == value::TypeTags::ksValue);

        _seekKeyHighHolder.reset(ownedHi, tagHi, valHi);

        // It is a point bound if the lowKey and highKey are same except discriminator.
        auto& highKey = *getSeekKeyHigh();
        _pointBound = getSeekKeyLow().compareWithoutDiscriminator(highKey) == 0 &&
            highKey.computeElementCount(*_ordering) == _entry->descriptor()->getNumFields();
    } else if (_seekKeyLow) {
        auto [ownedLow, tagLow, valLow] = _bytecode.run(_seekKeyLowCode.get());
        const auto msgTagLow = tagLow;
        uassert(4822853,
                str::stream() << "seek key is wrong type: " << msgTagLow,
                tagLow == value::TypeTags::ksValue);
        _seekKeyLowHolder.reset(ownedLow, tagLow, valLow);
    } else {
        auto sdi = _entry->accessMethod()->asSortedData()->getSortedDataInterface();
        key_string::Builder kb(sdi->getKeyStringVersion(),
                               sdi->getOrdering(),
                               key_string::Discriminator::kExclusiveBefore);
        kb.appendDiscriminator(key_string::Discriminator::kExclusiveBefore);

        auto [copyTag, copyVal] = value::makeCopyKeyString(kb.getValueCopy());
        _seekKeyLowHolder.reset(true, copyTag, copyVal);
    }
}

const key_string::Value& SimpleIndexScanStage::getSeekKeyLow() const {
    auto [tag, value] = _seekKeyLowHolder.getViewOfValue();
    return *value::getKeyStringView(value);
}

const key_string::Value* SimpleIndexScanStage::getSeekKeyHigh() const {
    if (!_seekKeyHigh) {
        return nullptr;
    }
    auto [tag, value] = _seekKeyHighHolder.getViewOfValue();
    return value::getKeyStringView(value);
}

std::unique_ptr<PlanStageStats> SimpleIndexScanStage::getStats(bool includeDebugInfo) const {
    auto stats = IndexScanStageBase::getStats(includeDebugInfo);
    if (includeDebugInfo && (_seekKeyLow || _seekKeyHigh)) {
        DebugPrinter printer;
        BSONObjBuilder bob(stats->debugInfo);
        if (_seekKeyLow) {
            bob.append("seekKeyLow", printer.print(_seekKeyLow->debugPrint()));
        }
        if (_seekKeyHigh) {
            bob.append("seekKeyHigh", printer.print(_seekKeyHigh->debugPrint()));
        }
        stats->debugInfo = bob.obj();
    }
    return stats;
}

std::vector<DebugPrinter::Block> SimpleIndexScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    if (_seekKeyLow) {
        DebugPrinter::addBlocks(ret, _seekKeyLow->debugPrint());
        if (_seekKeyHigh) {
            DebugPrinter::addBlocks(ret, _seekKeyHigh->debugPrint());
        } else {
            DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
        }
    }

    IndexScanStageBase::debugPrintImpl(ret);
    return ret;
}

size_t SimpleIndexScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += IndexScanStageBase::estimateCompileTimeSizeImpl();
    if (_seekKeyLow) {
        size += size_estimator::estimate(_seekKeyLow);
    }
    if (_seekKeyHigh) {
        size += size_estimator::estimate(_seekKeyHigh);
    }
    return size;
}

boost::optional<KeyStringEntry> SimpleIndexScanStage::seek() {
    return _cursor->seekForKeyString(getSeekKeyLow());
}

bool SimpleIndexScanStage::validateKey(const boost::optional<KeyStringEntry>& key) {
    if (!key) {
        _scanState = ScanState::kFinished;
        return false;
    }

    if (auto seekKeyHigh = getSeekKeyHigh(); seekKeyHigh) {
        auto cmp = key->keyString.compare(*seekKeyHigh);

        if (_forward) {
            if (cmp > 0) {
                _scanState = ScanState::kFinished;
                return false;
            }
        } else {
            if (cmp < 0) {
                _scanState = ScanState::kFinished;
                return false;
            }
        }
    }
    // Note: we may in the future want to bump 'keysExamined' for comparisons to a key that result
    // in the stage returning EOF.
    ++_specificStats.keysExamined;

    // For point bound on unique index, there's only one possible key.
    _scanState = _pointBound && _uniqueIndex ? ScanState::kFinished : ScanState::kScanning;
    return true;
}

GenericIndexScanStage::GenericIndexScanStage(UUID collUuid,
                                             StringData indexName,
                                             GenericIndexScanStageParams params,
                                             boost::optional<value::SlotId> indexKeySlot,
                                             boost::optional<value::SlotId> recordIdSlot,
                                             boost::optional<value::SlotId> snapshotIdSlot,
                                             boost::optional<value::SlotId> indexIdentSlot,
                                             IndexKeysInclusionSet indexKeysToInclude,
                                             value::SlotVector vars,
                                             PlanYieldPolicy* yieldPolicy,
                                             PlanNodeId planNodeId,
                                             bool participateInTrialRunTracking)
    : IndexScanStageBase("ixscan_generic"_sd,
                         collUuid,
                         indexName,
                         params.direction == 1,
                         indexKeySlot,
                         recordIdSlot,
                         snapshotIdSlot,
                         indexIdentSlot,
                         indexKeysToInclude,
                         std::move(vars),
                         yieldPolicy,
                         planNodeId,
                         participateInTrialRunTracking),
      _params{std::move(params)} {}

std::unique_ptr<PlanStage> GenericIndexScanStage::clone() const {
    sbe::GenericIndexScanStageParams params{_params.indexBounds->clone(),
                                            _params.keyPattern,
                                            _params.direction,
                                            _params.version,
                                            _params.ord};
    return std::make_unique<GenericIndexScanStage>(_collUuid,
                                                   _indexName,
                                                   std::move(params),
                                                   _indexKeySlot,
                                                   _recordIdSlot,
                                                   _snapshotIdSlot,
                                                   _indexIdentSlot,
                                                   _indexKeysToInclude,
                                                   _vars,
                                                   _yieldPolicy,
                                                   _commonStats.nodeId,
                                                   _participateInTrialRunTracking);
}

void GenericIndexScanStage::prepare(CompileCtx& ctx) {
    IndexScanStageBase::prepareImpl(ctx);
    ctx.root = this;
    _indexBoundsCode = _params.indexBounds->compile(ctx);
}

void GenericIndexScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    IndexScanStageBase::openImpl(reOpen);

    auto [ownedBound, tagBound, valBound] = _bytecode.run(_indexBoundsCode.get());
    if (tagBound == value::TypeTags::Nothing) {
        _scanState = ScanState::kFinished;
        return;
    }

    invariant(!ownedBound && tagBound == value::TypeTags::indexBounds,
              "indexBounds should be unowned and IndexBounds type");
    _checker.emplace(value::getIndexBoundsView(valBound), _params.keyPattern, _params.direction);

    if (!_checker->getStartSeekPoint(&_seekPoint)) {
        _scanState = ScanState::kFinished;
    }
}

std::vector<DebugPrinter::Block> GenericIndexScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();
    DebugPrinter::addBlocks(ret, _params.indexBounds->debugPrint());
    IndexScanStageBase::debugPrintImpl(ret);
    return ret;
}

size_t GenericIndexScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += IndexScanStageBase::estimateCompileTimeSizeImpl();
    size += size_estimator::estimate(_params.keyPattern);
    size += size_estimator::estimate(_params.indexBounds);
    size += size_estimator::estimate(_seekPoint);
    if (_checker) {
        size += size_estimator::estimate(*_checker);
    }
    size += size_estimator::estimate(_keyBuffer);
    return size;
}

boost::optional<KeyStringEntry> GenericIndexScanStage::seek() {
    return _cursor->seekForKeyString(IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
        _seekPoint, _params.version, _params.ord, _forward));
}

bool GenericIndexScanStage::validateKey(const boost::optional<KeyStringEntry>& key) {
    if (key && _checker) {
        ++_specificStats.keysExamined;

        _keyBuffer.reset();
        BSONObjBuilder keyBuilder(_keyBuffer);
        key_string::toBsonSafe(key->keyString.getBuffer(),
                               key->keyString.getSize(),
                               _params.ord,
                               key->keyString.getTypeBits(),
                               keyBuilder);
        auto bsonKey = keyBuilder.done();

        switch (_checker->checkKey(bsonKey, &_seekPoint)) {
            case IndexBoundsChecker::VALID:
                _scanState = ScanState::kScanning;
                return true;

            case IndexBoundsChecker::DONE:
                _scanState = ScanState::kFinished;
                return false;

            case IndexBoundsChecker::MUST_ADVANCE:
                _scanState = ScanState::kNeedSeek;
                return false;
        }
    }

    _scanState = ScanState::kFinished;
    return false;
}
}  // namespace mongo::sbe
