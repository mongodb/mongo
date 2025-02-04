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

#include "mongo/db/exec/recordid_deduplicator.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/pipeline/spilling/record_store_batch_writer.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"

namespace mongo {

Counter64& roaringMetric =
    *MetricBuilder<Counter64>{"query.recordIdDeduplicationSwitchedToRoaring"};

RecordIdDeduplicator::RecordIdDeduplicator(ExpressionContext* expCtx,
                                           size_t threshold,
                                           size_t chunkSize,
                                           uint64_t universeSize)
    : _expCtx(expCtx),
      _roaring(threshold, chunkSize, universeSize, [&]() { roaringMetric.increment(); }),
      _memoryTracker{expCtx->getAllowDiskUse() && !expCtx->getInRouter(),
                     std::numeric_limits<long long>::max()} {}

bool RecordIdDeduplicator::contains(const RecordId& recordId) const {
    return recordId.withFormat(
        [&](RecordId::Null _) -> bool { return _hashset.contains(recordId); },
        [&](int64_t rid) -> bool { return _roaring.contains(rid); },
        [&](const char* str, int size) -> bool { return _hashset.contains(recordId); });
}

bool RecordIdDeduplicator::insert(const RecordId& recordId) {
    RecordData record;

    bool foundInMemory = recordId.withFormat(
        [&](RecordId::Null _) -> bool { return hasNullRecordId; },
        [&](int64_t rid) -> bool { return _roaring.contains(rid); },
        [&](const char* str, int size) -> bool { return _hashset.contains(recordId); });

    if (foundInMemory) {
        return false;
    }

    bool foundInDisk = hasSpilled() &&
        recordId.withFormat([&](RecordId::Null _) -> bool { return hasNullRecordId; },
                            [&](int64_t rid) -> bool {
                                return _diskStorageLong &&
                                    _expCtx->getMongoProcessInterface()->checkRecordInRecordStore(
                                        _expCtx, _diskStorageLong->rs(), recordId);
                            },
                            [&](const char* str, int size) -> bool {
                                return _diskStorageString &&
                                    _expCtx->getMongoProcessInterface()->checkRecordInRecordStore(
                                        _expCtx, _diskStorageString->rs(), recordId);
                            });

    if (foundInDisk) {
        return false;
    }

    // The record was found neither in memory nor in disk. Insert it.
    recordId.withFormat([&](RecordId::Null _) { hasNullRecordId = true; },
                        [&](int64_t rid) { _roaring.addChecked(rid); },
                        [&](const char* str, int size) {
                            _hashset.insert(recordId);
                            _memoryTracker.add(recordId.memUsage() + 2 * sizeof(void*));
                        });


    // ToDo: SERVER-99279 Check memory and spill.
    return true;
}

void RecordIdDeduplicator::spill(uint64_t maximumMemoryUsageBytes) {
    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            str::stream() << "Exceeded memory limit of " << maximumMemoryUsageBytes
                          << ", but didn't allow external sort."
                             " Pass allowDiskUse:true to opt in.",
            _expCtx->getAllowDiskUse());

    uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
        storageGlobalParams.dbpath,
        _expCtx->getQueryKnobConfiguration().getInternalQuerySpillingMinAvailableDiskSpaceBytes()));

    uint64_t additionalSpilledBytes{0};
    uint64_t additionalSpilledRecords{0};
    uint64_t currentSpilledDataStorageSize{0};

    if (!_hashset.empty()) {
        // For string recordId.
        if (!_diskStorageString) {
            _diskStorageString = _expCtx->getMongoProcessInterface()->createTemporaryRecordStore(
                _expCtx, KeyFormat::String);
        }

        RecordStoreBatchWriter writer{_expCtx, _diskStorageString->rs()};
        for (auto it = _hashset.begin(); it != _hashset.end(); ++it) {
            writer.write(*it, RecordData{});
        }

        // Flush the remaining records.
        writer.flush();
        additionalSpilledBytes += writer.writtenBytes();
        additionalSpilledRecords += writer.writtenRecords();

        _hashset.clear();
    }

    if (!_roaring.empty()) {
        // For long recordId.
        if (!_diskStorageLong) {
            _diskStorageLong = _expCtx->getMongoProcessInterface()->createTemporaryRecordStore(
                _expCtx, KeyFormat::Long);
        }

        RecordStoreBatchWriter writer{_expCtx, _diskStorageLong->rs()};
        for (auto it = _roaring.begin(); it != _roaring.end(); ++it) {
            writer.write(RecordId(*it), RecordData{});
        }

        // Flush the remaining records.
        writer.flush();
        additionalSpilledBytes += writer.writtenBytes();
        additionalSpilledRecords += writer.writtenRecords();

        _roaring.clear();
    }

    _memoryTracker.resetCurrent();

    if (_diskStorageLong) {
        currentSpilledDataStorageSize += _diskStorageLong->rs()->storageSize(
            *shard_role_details::getRecoveryUnit(_expCtx->getOperationContext()));
    };

    if (_diskStorageString) {
        currentSpilledDataStorageSize += _diskStorageString->rs()->storageSize(
            *shard_role_details::getRecoveryUnit(_expCtx->getOperationContext()));
    };

    _stats.updateSpillingStats(
        1, additionalSpilledBytes, additionalSpilledRecords, currentSpilledDataStorageSize);
}

}  // namespace mongo
