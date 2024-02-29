/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/hashagg_base.h"

#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/stages/block_hashagg.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/stats/counters.h"

namespace mongo {
namespace sbe {

template <class Derived>
HashAggBaseStage<Derived>::HashAggBaseStage(StringData stageName,
                                            PlanNodeId planNodeId,
                                            value::SlotAccessor* collatorAccessor,
                                            bool participateInTrialRunTracking,
                                            bool allowDiskUse,
                                            bool forceIncreasedSpilling)
    : PlanStage(stageName, planNodeId, participateInTrialRunTracking),
      _collatorAccessor(collatorAccessor),
      _allowDiskUse(allowDiskUse),
      _forceIncreasedSpilling(forceIncreasedSpilling) {
    if (_forceIncreasedSpilling) {
        tassert(7039554, "'forceIncreasedSpilling' set but disk use not allowed", _allowDiskUse);
    }
}

template <class Derived>
HashAggBaseStage<Derived>::~HashAggBaseStage() {
    auto* specificStats = static_cast<Derived*>(this)->getHashAggStats();
    groupCounters.incrementGroupCounters(specificStats->spills,
                                         specificStats->spilledDataStorageSize,
                                         specificStats->spilledRecords);
}

template <class Derived>
void HashAggBaseStage<Derived>::doSaveState(bool relinquishCursor) {
    if (relinquishCursor) {
        if (_rsCursor) {
            _recordStore->saveCursor(_opCtx, _rsCursor);
        }
    }
    if (_rsCursor) {
        _rsCursor->setSaveStorageCursorOnDetachFromOperationContext(!relinquishCursor);
    }

    if (_recordStore) {
        _recordStore->saveState();
    }
}

template <class Derived>
void HashAggBaseStage<Derived>::doRestoreState(bool relinquishCursor) {
    invariant(_opCtx);
    if (_recordStore) {
        _recordStore->restoreState();
    }

    if (_rsCursor && relinquishCursor) {
        auto couldRestore = _recordStore->restoreCursor(_opCtx, _rsCursor);
        uassert(6196500, "HashAggStage could not restore cursor", couldRestore);
    }
}

template <class Derived>
void HashAggBaseStage<Derived>::doDetachFromOperationContext() {
    if (_rsCursor) {
        _rsCursor->detachFromOperationContext();
    }
}

template <class Derived>
void HashAggBaseStage<Derived>::doAttachToOperationContext(OperationContext* opCtx) {
    if (_rsCursor) {
        _rsCursor->reattachToOperationContext(opCtx);
    }
}

template <class Derived>
typename HashAggBaseStage<Derived>::SpilledRow HashAggBaseStage<Derived>::deserializeSpilledRecord(
    const Record& record, size_t numKeys, BufBuilder& keyBuffer) {
    // Read the values and type bits out of the value part of the record.
    BufReader valReader(record.data.data(), record.data.size());

    auto val = value::MaterializedRow::deserializeForSorter(valReader, {nullptr /*collator*/});
    auto typeBits =
        key_string::TypeBits::fromBuffer(key_string::Version::kLatestVersion, &valReader);

    keyBuffer.reset();
    auto key = value::MaterializedRow::deserializeFromKeyString(
        decodeKeyString(record.id, typeBits), &keyBuffer, numKeys /*numPrefixValuesToRead*/);
    return std::make_pair(std::move(key), std::move(val));
}

template <class Derived>
void HashAggBaseStage<Derived>::makeTemporaryRecordStore() {
    tassert(
        5907500,
        "HashAggStage attempted to write to disk in an environment which is not prepared to do so",
        _opCtx->getServiceContext());
    tassert(5907501,
            "No storage engine so HashAggStage cannot spill to disk",
            _opCtx->getServiceContext()->getStorageEngine());
    assertIgnorePrepareConflictsBehavior(_opCtx);
    _recordStore = std::make_unique<SpillingStore>(_opCtx);

    static_cast<Derived*>(this)->getHashAggStats()->usedDisk = true;
}

template <class Derived>
void HashAggBaseStage<Derived>::spillRowToDisk(const value::MaterializedRow& key,
                                               const value::MaterializedRow& val) {
    CollatorInterface* collator = nullptr;
    if (_collatorAccessor) {
        auto [colTag, colVal] = _collatorAccessor->getViewOfValue();
        collator = value::getCollatorView(colVal);
    }

    key_string::Builder kb{key_string::Version::kLatestVersion};
    // Serialize the key that will be used as the record id (rid) when storing the record in the
    // record store. Use a keystring for the spilled entry's rid such that partial aggregates are
    // guaranteed to have identical keystrings when their keys are equal with respect to the
    // collation.
    key.serializeIntoKeyString(kb, collator);
    // Add a unique integer to the end of the key, since record ids must be unique. We want equal
    // keys to be adjacent in the 'RecordStore' so that we can merge the partial aggregates with a
    // single pass.
    kb.appendNumberLong(_ridSuffixCounter++);
    auto rid = RecordId(kb.getBuffer(), kb.getSize());

    if (collator) {
        // The keystring cannot always be deserialized back to the original keys when a collation is
        // in use, so we also store the unmodified key in the data part of the spilled record.
        _recordStore->upsertToRecordStore(_opCtx, rid, key, val, false /*update*/);
    } else {
        auto typeBits = kb.getTypeBits();
        _recordStore->upsertToRecordStore(_opCtx, rid, val, typeBits, false /*update*/);
    }

    static_cast<Derived*>(this)->getHashAggStats()->spilledRecords++;
}

template <class Derived>
void HashAggBaseStage<Derived>::spill(MemoryCheckData& mcd) {
    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            "Exceeded memory limit for $group, but didn't allow external spilling;"
            " pass allowDiskUse:true to opt in",
            _allowDiskUse);

    // Since we flush the entire hash table to disk, we also clear any state related to estimating
    // memory consumption.
    mcd.reset();

    if (!_recordStore) {
        makeTemporaryRecordStore();
    }

    for (auto&& it : *_ht) {
        spillRowToDisk(it.first, it.second);
    }

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
    // We're not actually doing any sorting here or using the 'Sorter' class, but for the purposes
    // of $operationMetrics we incorporate the number of spilled records into the "keysSorted"
    // metric. Similarly, "sorterSpills" despite the name counts the number of individual spill
    // events.
    metricsCollector.incrementKeysSorted(_ht->size());
    metricsCollector.incrementSorterSpills(1);

    _ht->clear();

    static_cast<Derived*>(this)->getHashAggStats()->spills++;
}

// Checks memory usage. Ideally, we'd want to know the exact size of already accumulated data, but
// we cannot, so we estimate it based on the last updated/inserted row, if we have one, or the first
// row in the '_ht' table. If the estimated memory usage exceeds the allowed, this method initiates
// spilling.
template <class Derived>
void HashAggBaseStage<Derived>::checkMemoryUsageAndSpillIfNecessary(MemoryCheckData& mcd,
                                                                    bool emptyInKeyAccessors) {
    invariant(!_ht->empty());

    // If the group-by key is empty we will only ever aggregate into a single row so no sense in
    // spilling.
    if (emptyInKeyAccessors) {
        return;
    }

    mcd.memoryCheckpointCounter++;
    if (mcd.memoryCheckpointCounter < mcd.nextMemoryCheckpoint) {
        // We haven't reached the next checkpoint at which we estimate memory usage and decide if we
        // should spill.
        return;
    }

    const long estimatedRowSize =
        _htIt->first.memUsageForSorter() + _htIt->second.memUsageForSorter();
    const long long estimatedTotalSize = _ht->size() * estimatedRowSize;

    if (estimatedTotalSize >= _approxMemoryUseInBytesBeforeSpill) {
        spill(mcd);
    } else {
        // Calculate the next memory checkpoint. We estimate it based on the prior growth of the
        // '_ht' and the remaining available memory. If 'estimatedGainPerChildAdvance' suggests that
        // the hash table is growing, then the checkpoint is estimated as some configurable
        // percentage of the number of additional input rows that we would have to process to
        // consume the remaining memory. On the other hand, a value of 'estimtedGainPerChildAdvance'
        // close to zero indicates a stable hash stable size, in which case we can delay the next
        // check progressively.
        const double estimatedGainPerChildAdvance =
            (static_cast<double>(estimatedTotalSize - mcd.lastEstimatedMemoryUsage) /
             mcd.memoryCheckpointCounter);

        const long nextCheckpointCandidate = (estimatedGainPerChildAdvance > 0.1)
            ? mcd.checkpointMargin * (_approxMemoryUseInBytesBeforeSpill - estimatedTotalSize) /
                estimatedGainPerChildAdvance
            : mcd.nextMemoryCheckpoint * 2;

        mcd.nextMemoryCheckpoint =
            std::min<long>(mcd.memoryCheckFrequency,
                           std::max<long>(mcd.atMostCheckFrequency, nextCheckpointCandidate));

        mcd.lastEstimatedMemoryUsage = estimatedTotalSize;
        mcd.memoryCheckpointCounter = 0;
        mcd.memoryCheckFrequency =
            std::min<long>(mcd.memoryCheckFrequency * 2, mcd.atLeastMemoryCheckFrequency);
    }
}

template <class Derived>
void HashAggBaseStage<Derived>::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

template <class Derived>
PlanStage::TrialRunTrackerAttachResultMask HashAggBaseStage<Derived>::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    // The BlockHashAggStage only tracks the "numResults" metric when it is the most deeply nested
    // blocking stage.
    if (!(childrenAttachResult & TrialRunTrackerAttachResultFlags::AttachedToBlockingStage)) {
        _tracker = tracker;
    }

    // Return true to indicate that the tracker is attached to a blocking stage: either this stage
    // or one of its descendent stages.
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToBlockingStage;
}

template class HashAggBaseStage<HashAggStage>;
template class HashAggBaseStage<BlockHashAggStage>;
}  // namespace sbe
}  // namespace mongo
