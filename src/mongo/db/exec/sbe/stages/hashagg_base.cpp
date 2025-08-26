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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/stages/block_hashagg.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_options.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace sbe {

template <class Derived>
HashAggBaseStage<Derived>::HashAggBaseStage(StringData stageName,
                                            PlanYieldPolicy* yieldPolicy,
                                            PlanNodeId planNodeId,
                                            value::SlotAccessor* collatorAccessor,
                                            bool participateInTrialRunTracking,
                                            bool allowDiskUse,
                                            bool forceIncreasedSpilling)
    : PlanStage(stageName, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _collatorAccessor(collatorAccessor),
      _allowDiskUse(allowDiskUse),
      _forceIncreasedSpilling(forceIncreasedSpilling) {
    if (_forceIncreasedSpilling) {
        tassert(7039554, "'forceIncreasedSpilling' set but disk use not allowed", _allowDiskUse);
    }
}

template <class Derived>
void HashAggBaseStage<Derived>::doSaveState() {
    if (_rsCursor) {
        _recordStore->saveCursor(_opCtx, _rsCursor);
    }

    if (_recordStore) {
        _recordStore->saveState();
    }
}

template <class Derived>
void HashAggBaseStage<Derived>::doRestoreState() {
    invariant(_opCtx);
    if (_recordStore) {
        _recordStore->restoreState();
    }

    if (_recordStore && _rsCursor) {
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
int64_t HashAggBaseStage<Derived>::spillRowToDisk(const value::MaterializedRow& key,
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
    auto rid = RecordId(kb.getView());

    int spilledBytes = 0;
    if (collator) {
        // The keystring cannot always be deserialized back to the original keys when a collation is
        // in use, so we also store the unmodified key in the data part of the spilled record.
        spilledBytes = _recordStore->upsertToRecordStore(_opCtx, rid, key, val, false /*update*/);
    } else {
        auto typeBits = kb.getTypeBits();
        spilledBytes =
            _recordStore->upsertToRecordStore(_opCtx, rid, val, typeBits, false /*update*/);
    }

    return spilledBytes;
}

template <class Derived>
void HashAggBaseStage<Derived>::spill() {
    // The stage returns results using an iterator '_htIt' over the hashTable '_ht'. At any moment
    // '_htIt' points to the record that should be returned in the next getNext() invocation. When
    // we spill, we want to spill only the records in '_ht' that have not been already returned to
    // the caller.
    if (_htIt == _ht->end()) {
        LOGV2_DEBUG(9915700,
                    2,
                    "All in memory data has been consumed. HashAgg stage has nothing to spill. "
                    "Clearing memory.");
        _ht->clear();
        _htIt = _ht->end();
        _memoryTracker.value().set(0);
        return;
    }

    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            "Exceeded memory limit for $group, but didn't allow external spilling;"
            " pass allowDiskUse:true to opt in",
            _allowDiskUse);

    // Ensure there is sufficient disk space for spilling
    uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
        storageGlobalParams.dbpath, internalQuerySpillingMinAvailableDiskSpaceBytes.load()));

    if (!_recordStore) {
        makeTemporaryRecordStore();
    }

    int64_t spilledBytes = 0;
    int64_t spilledRecords = 0;

    // Spill only the records that have not been already consumed.
    for (; _htIt != _ht->end(); ++_htIt) {
        spilledBytes += spillRowToDisk(_htIt->first, _htIt->second);
        spilledRecords++;
    }

    _ht->clear();
    _htIt = _ht->end();
    _memoryTracker.value().set(0);

    auto spilledDataStorageIncrease =
        static_cast<Derived*>(this)->getHashAggStats()->spillingStats.updateSpillingStats(
            1 /* spills */, spilledBytes, spilledRecords, _recordStore->storageSize(_opCtx));
    groupCounters.incrementPerSpilling(
        1 /* spills */, spilledBytes, spilledRecords, spilledDataStorageIncrease);
    _recordStore->updateSpillStorageStatsForOperation(_opCtx);
}

template <class Derived>
void HashAggBaseStage<Derived>::spill(MemoryCheckData& mcd) {
    spill();

    // Since we flush the entire hash table to disk, we also clear any state related to estimating
    // memory consumption.
    mcd.reset();
}

template <class Derived>
void HashAggBaseStage<Derived>::doForceSpill() {
    // The state has already finished (_ht is set in open and unset in close)
    if (!_ht) {
        LOGV2_DEBUG(9915601, 2, "HashAggStage has finished its execution");
        return;
    }

    // If we've already spilled, then there is nothing else to do.
    if (_recordStore) {
        return;
    }

    // Check before advancing _htIt.
    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            "$group Received a spilling request, but didn't allow external spilling;"
            " pass allowDiskUse:true to opt in",
            _allowDiskUse);

    static_assert(
        std::is_member_function_pointer_v<decltype(&Derived::setIteratorToNextRecord)>,
        "A class derived from HashAggBaseStage must implement 'setIteratorToNextRecord' method");

    derived().setIteratorToNextRecord();

    spill();

    if (_recordStore) {
        static_assert(std::is_member_function_pointer_v<decltype(&Derived::switchToDisk)>,
                      "A class derived from HashAggBaseStage must implement 'switchToDisk' method");

        derived().switchToDisk();
    }

    doSaveState();
}

// Checks memory usage. Ideally, we'd want to know the exact size of already accumulated data, but
// we cannot, so we estimate it based on the last updated/inserted row, if we have one, or the first
// row in the '_ht' table. If the estimated memory usage exceeds the allowed, this method initiates
// spilling.
template <class Derived>
void HashAggBaseStage<Derived>::checkMemoryUsageAndSpillIfNecessary(MemoryCheckData& mcd) {
    invariant(!_ht->empty());

    mcd.memoryCheckpointCounter++;
    if (mcd.memoryCheckpointCounter < mcd.nextMemoryCheckpoint) {
        // We haven't reached the next checkpoint at which we estimate memory usage and decide if we
        // should spill.
        return;
    }

    const long lastEstimatedMemoryUsage = _memoryTracker.value().inUseTrackedMemoryBytes();
    const long estimatedRowSize =
        _htIt->first.memUsageForSorter() + _htIt->second.memUsageForSorter();
    _memoryTracker.value().set(_ht->size() * estimatedRowSize);
    static_cast<Derived*>(this)->getHashAggStats()->peakTrackedMemBytes =
        _memoryTracker.value().peakTrackedMemoryBytes();

    if (!_memoryTracker.value().withinMemoryLimit()) {
        // It is safe to set this to the begining because spilling outside the releaseMemory only
        // happens before any results have been consumed and every time data is spilled the _ht is
        // cleared.
        _htIt = _ht->begin();
        spill(mcd);
    } else {
        // Calculate the next memory checkpoint. We estimate it based on the prior growth of the
        // '_ht' and the remaining available memory. If 'estimatedGainPerChildAdvance' suggests that
        // the hash table is growing, then the checkpoint is estimated as some configurable
        // percentage of the number of additional input rows that we would have to process to
        // consume the remaining memory. On the other hand, a value of
        // 'estimatedGainPerChildAdvance' close to zero indicates a stable hash stable size, in
        // which case we can delay the next check progressively.
        const double estimatedGainPerChildAdvance =
            (static_cast<double>(_memoryTracker.value().inUseTrackedMemoryBytes() -
                                 lastEstimatedMemoryUsage) /
             mcd.memoryCheckpointCounter);

        const long nextCheckpointCandidate = (estimatedGainPerChildAdvance > 0.1)
            ? mcd.checkpointMargin *
                (_memoryTracker.value().maxAllowedMemoryUsageBytes() -
                 _memoryTracker.value().inUseTrackedMemoryBytes()) /
                estimatedGainPerChildAdvance
            : mcd.nextMemoryCheckpoint * 2;

        mcd.nextMemoryCheckpoint =
            std::min<long>(mcd.memoryCheckFrequency,
                           std::max<long>(mcd.atMostCheckFrequency, nextCheckpointCandidate));

        mcd.memoryCheckpointCounter = 0;
        mcd.memoryCheckFrequency =
            std::min<long>(mcd.memoryCheckFrequency * 2, mcd.atLeastMemoryCheckFrequency);
    }
}

template class HashAggBaseStage<HashAggStage>;
template class HashAggBaseStage<BlockHashAggStage>;
}  // namespace sbe
}  // namespace mongo
