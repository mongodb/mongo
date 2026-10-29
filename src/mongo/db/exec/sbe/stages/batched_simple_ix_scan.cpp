// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/batched_simple_ix_scan.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <utility>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

BatchedSimpleIndexScanStage::BatchedSimpleIndexScanStage(
    UUID collUuid,
    DatabaseName dbName,
    std::string_view indexName,
    size_t maxIndexKeyBatchCount,
    boost::optional<value::SlotId> recordIdSlot,
    boost::optional<value::SlotId> snapshotIdSlot,
    boost::optional<value::SlotId> indexIdentSlot,
    std::unique_ptr<EExpression> seekKeyLow,
    std::unique_ptr<EExpression> seekKeyHigh,
    PlanYieldPolicySBE* yieldPolicy,
    PlanNodeId nodeId,
    bool participateInTrialRunTracking)
    : IndexScanStageBase(seekKeyLow ? "batched_ixseek"sv : "batched_ixscan"sv,
                         collUuid,
                         dbName,
                         indexName,
                         /*forward*/ true,
                         /*indexKeySlot*/ boost::none,
                         recordIdSlot,
                         snapshotIdSlot,
                         indexIdentSlot,
                         IndexKeysInclusionSet{},
                         value::SlotVector{},
                         yieldPolicy,
                         nodeId,
                         participateInTrialRunTracking),
      _maxIndexKeyBatchCount(maxIndexKeyBatchCount),
      _seekKeyLow(std::move(seekKeyLow)),
      _seekKeyHigh(std::move(seekKeyHigh)) {
    tassert(13624801, "Expecting maxIndexKeyBatchCount to be positive", _maxIndexKeyBatchCount > 0);
    tassert(13624802,
            "Expecting neither boundary keys, only low boundary key, or both low and high boundary "
            "key to be set",
            _seekKeyLow || !_seekKeyHigh);
}

std::unique_ptr<PlanStage> BatchedSimpleIndexScanStage::clone() const {
    return std::make_unique<BatchedSimpleIndexScanStage>(
        _collUuid,
        _dbName,
        _indexName,
        _maxIndexKeyBatchCount,
        _recordIdSlot,
        _snapshotIdSlot,
        _indexIdentSlot,
        _seekKeyLow ? _seekKeyLow->clone() : nullptr,
        _seekKeyHigh ? _seekKeyHigh->clone() : nullptr,
        _yieldPolicy,
        _commonStats.nodeId,
        participateInTrialRunTracking());
}

void BatchedSimpleIndexScanStage::prepare(CompileCtx& ctx) {
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

void BatchedSimpleIndexScanStage::doSaveState() {
    // Seek points are external to the index scan and must be accessible no matter what as long
    // as the index scan is opened.
    if (_open) {
        if (_seekKeyLow) {
            prepareForYielding(_seekKeyLowHolder, true);
        }
        if (_seekKeyHigh) {
            prepareForYielding(_seekKeyHighHolder, true);
        }
    }

    IndexScanStageBase::doSaveState();
}

void BatchedSimpleIndexScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    IndexScanStageBase::openImpl(reOpen);

    if (_seekKeyLow && _seekKeyHigh) {
        initializeSeekKeyLow();
        initializeSeekKeyHigh();

        _cursor->setEndPosition(getSeekKeyHigh());
    } else if (_seekKeyLow) {
        initializeSeekKeyLow();
    } else {
        initializeSeekKeyDefault();
    }
}

bool BatchedSimpleIndexScanStage::advanceToNextKey(RecoveryUnit& ru) {
    switch (_scanState) {
        case ScanState::kNeedSeek:
            ++_specificStats.seeks;
            _nextKeyString = seek(ru);
            break;
        case ScanState::kScanning:
            _nextKeyString = _cursor->nextKeyValueView(ru);
            break;
        case ScanState::kFinished:
            return false;
    }
    trackIndexRead();

    if (_nextKeyString.isEmpty()) {
        _scanState = ScanState::kFinished;
        return false;
    }

    ++_specificStats.keysExamined;
    _scanState = ScanState::kScanning;
    return true;
}

PlanState BatchedSimpleIndexScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to get next records from a storage cursor so do not bother saving our internal
    // state in case it yields as the state will be completely overwritten after the calls.
    disableSlotAccess();

    // TODO SERVER-136754: Check for interrupts within the batch loop and determine if it's also
    // necessary to check for yields.
    checkForInterruptAndYield(_opCtx);

    auto& ru = *shard_role_details::getRecoveryUnit(_opCtx);

    std::vector<int64_t> recordIds;
    recordIds.reserve(_maxIndexKeyBatchCount);
    while (recordIds.size() < _maxIndexKeyBatchCount && advanceToNextKey(ru)) {
        const RecordId* nextRid = _nextKeyString.getRecordId();
        tassert(13624803,
                "BatchedSimpleIndexScanStage only supports integer record ids",
                nextRid && nextRid->isLong());
        recordIds.push_back(nextRid->getLong());
    }

    if (recordIds.empty()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    if (_recordIdSlot) {
        if (_maxIndexKeyBatchCount > 1) {
            auto block = std::make_unique<value::Int64RecordIdBlock>(std::move(recordIds));
            _recordIdAccessor.reset(value::TagValueOwned::fromRaw(
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(block.release())));
        } else {
            // '_nextKeyString' still refers to the only key we read, so we can view its RecordId.
            _recordIdAccessor.reset(value::TagValueView{
                value::TypeTags::RecordId,
                value::bitcastFrom<const RecordId*>(_nextKeyString.getRecordId())});
        }
    }

    if (_snapshotIdSlot) {
        // Copy the latest snapshot ID into the 'snapshotId' slot.
        _snapshotIdAccessor.reset(value::TypeTags::NumberInt64,
                                  value::bitcastFrom<uint64_t>(_latestSnapshotId));
    }

    return trackPlanState(PlanState::ADVANCED);
}

const key_string::Value& BatchedSimpleIndexScanStage::getSeekKeyLow() const {
    auto [tag, value] = _seekKeyLowHolder.getViewOfValue();
    return value::getKeyString(value)->getValue();
}

const key_string::Value& BatchedSimpleIndexScanStage::getSeekKeyHigh() const {
    tassert(13624804, "HighKey must exist", _seekKeyHigh);
    auto [tag, value] = _seekKeyHighHolder.getViewOfValue();
    return value::getKeyString(value)->getValue();
}

void BatchedSimpleIndexScanStage::initializeSeekKeyLow() {
    value::TagValueMaybeOwned seekKeyLow = _bytecode->run(_seekKeyLowCode.get());
    uassert(13624805,
            str::stream() << "seek key is wrong type: " << seekKeyLow.tag(),
            seekKeyLow.tag() == value::TypeTags::keyString);
    _seekKeyLowHolder.reset(std::move(seekKeyLow));
}

void BatchedSimpleIndexScanStage::initializeSeekKeyHigh() {
    value::TagValueMaybeOwned seekKeyHigh = _bytecode->run(_seekKeyHighCode.get());
    uassert(13624806,
            str::stream() << "seek key is wrong type: " << seekKeyHigh.tag(),
            seekKeyHigh.tag() == value::TypeTags::keyString);
    _seekKeyHighHolder.reset(std::move(seekKeyHigh));
}

void BatchedSimpleIndexScanStage::initializeSeekKeyDefault() {
    auto sdi = _entry->accessMethod()->asSortedData()->getSortedDataInterface();
    key_string::Builder kb(sdi->getKeyStringVersion(),
                           sdi->getOrdering(),
                           key_string::Discriminator::kExclusiveBefore);
    kb.appendDiscriminator(key_string::Discriminator::kExclusiveBefore);

    _seekKeyLowHolder.reset(value::TagValueOwned::fromRaw(value::makeKeyString(kb.getValueCopy())));
}

std::unique_ptr<PlanStageStats> BatchedSimpleIndexScanStage::getStats(bool includeDebugInfo) const {
    auto stats = IndexScanStageBase::getStats(includeDebugInfo);
    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob(stats->debugInfo);
        bob.appendNumber("maxIndexKeyBatchCount", static_cast<long long>(_maxIndexKeyBatchCount));
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

void BatchedSimpleIndexScanStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                               DebugPrintInfo& debugPrintInfo) const {
    if (_seekKeyLow) {
        DebugPrinter::addKeyword(ret, "seekKeyLow");
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, _seekKeyLow->debugPrint());
        if (_seekKeyHigh) {
            DebugPrinter::addKeyword(ret, "seekKeyHigh");
            ret.emplace_back("=");
            DebugPrinter::addBlocks(ret, _seekKeyHigh->debugPrint());
        }
    }

    DebugPrinter::addKeyword(ret, "maxIndexKeyBatchCount");
    ret.emplace_back("=");
    ret.emplace_back(std::to_string(_maxIndexKeyBatchCount));

    IndexScanStageBase::debugPrintImpl(ret);

    if (debugPrintInfo.printBytecode) {
        DebugPrinter::addNewLine(ret);
        PlanStage::debugPrintBytecode(ret, _seekKeyLowCode, /*title=*/"SEEK_KEY_LOW");
        PlanStage::debugPrintBytecode(ret, _seekKeyHighCode, /*title=*/"SEEK_KEY_HIGH");
    }
}

size_t BatchedSimpleIndexScanStage::estimateCompileTimeSize() const {
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

}  // namespace mongo::sbe
