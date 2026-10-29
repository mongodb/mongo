// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/recordid_deduplicator.h"

#include "mongo/db/commands/server_status/server_status_metric.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

Counter64& roaringMetric =
    *MetricBuilder<Counter64>{"query.recordIdDeduplicationSwitchedToRoaring"};

RecordIdDeduplicator::RecordIdDeduplicator(ExpressionContext* expCtx,
                                           size_t threshold,
                                           size_t chunkSize,
                                           uint64_t universeSize)
    : _expCtx(expCtx),
      _roaring(threshold, chunkSize, universeSize, [&]() { roaringMetric.increment(); }) {}

bool RecordIdDeduplicator::contains(const RecordId& recordId) const {
    bool foundInMemory = recordId.withFormat(
        [&](RecordId::Null _) -> bool { return hasNullRecordId; },
        [&](int64_t rid) -> bool { return _roaring.contains(rid); },
        [&](const char* str, int size) -> bool { return _hashset.contains(recordId); });

    if (foundInMemory) {
        return true;
    }

    bool foundInDisk = hasSpilled() &&
        recordId.withFormat([&](RecordId::Null _) -> bool { return hasNullRecordId; },
                            [&](int64_t rid) -> bool {
                                return _diskStorageLong &&
                                    _expCtx->getMongoProcessInterface()->checkRecordInSpillTable(
                                        _expCtx, *_diskStorageLong, recordId);
                            },
                            [&](const char* str, int size) -> bool {
                                return _diskStorageString &&
                                    _expCtx->getMongoProcessInterface()->checkRecordInSpillTable(
                                        _expCtx, *_diskStorageString, recordId);
                            });

    return foundInDisk;
}

bool RecordIdDeduplicator::insert(const RecordId& recordId) {
    if (contains(recordId)) {
        return false;
    }

    recordId.withFormat([&](RecordId::Null _) { hasNullRecordId = true; },
                        [&](int64_t rid) { _roaring.addChecked(rid); },
                        [&](const char* str, int size) {
                            _hashSetMemUsage += recordId.memUsage();
                            _hashset.insert(recordId);
                        });

    return true;
}

void RecordIdDeduplicator::freeMemory(const RecordId& recordId) {
    recordId.withFormat([&](RecordId::Null _) { hasNullRecordId = false; },
                        [&](int64_t rid) { _roaring.erase(rid); },
                        [&](const char* str, int size) {
                            if (_hashset.erase(recordId)) {
                                uassert(10762700,
                                        str::stream()
                                            << "Trying to remove a recordId of size "
                                            << recordId.memUsage()
                                            << " from a hashset of total size " << _hashSetMemUsage,
                                        _hashSetMemUsage >= recordId.memUsage());
                                _hashSetMemUsage -= recordId.memUsage();
                            }
                        });
}

void RecordIdDeduplicator::spill(SpillingStats& stats, uint64_t maximumMemoryUsageBytes) {
    LOGV2_DEBUG(11001900, 2, "RecordIdDeduplicator does not spill.");
}

}  // namespace mongo
