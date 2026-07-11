// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/spilling/spilling_stats.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/util/hash_roaring_set.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * RecordId deduplicator optimized for memory usage: it uses Roaring Bitmaps to store large number
 * of integer RecordIds and hash table otherwise.
 */
class RecordIdDeduplicator {
public:
    /**
     * See HashRoaringSet constructor for definitions of the 'threshold', 'chunkSize', and
     * 'universeSize' parameters needed in the definition of HashRoaringSet.
     */
    RecordIdDeduplicator(ExpressionContext* expCtx,
                         size_t threshold,
                         size_t chunkSize,
                         uint64_t universeSize);

    RecordIdDeduplicator(ExpressionContext* expCtx)
        : RecordIdDeduplicator(expCtx,
                               static_cast<size_t>(internalRoaringBitmapsThreshold.load()),
                               static_cast<size_t>(internalRoaringBitmapsBatchSize.load()),
                               static_cast<uint64_t>(internalRoaringBitmapsThreshold.load() /
                                                     internalRoaringBitmapsMinimalDensity.load())) {
    }

    bool hasSpilled() const {
        return (_diskStorageString || _diskStorageLong);
    }


    /**
     * Return true if the RecordId has been seen.
     */
    bool contains(const RecordId& recordId) const;

    /**
     * Insert a RecordId and return true if the RecordId has not been seen.
     */
    bool insert(const RecordId& recordId);

    /**
     * Spills to disk until the memory usage does not exceed maximumMemoryUsageBytes and updates the
     * provided SpillingStats. If no value is provided for maximumMemoryUsageBytes then it spills
     * everything.
     */
    void spill(SpillingStats& stats, uint64_t maximumMemoryUsageBytes = 0);

    /**
     * Removes the recordId from in-memory structures. This might not actually remove recordId from
     * the deduplicator, if the record is spilled to disk.
     * This function should only be used to free memory if this recordId is guaranteed not to be
     * encountered again.
     */
    void freeMemory(const RecordId& recordId);

    uint64_t getApproximateSize() const {
        size_t emptySlotsSpace = (_hashset.capacity() - _hashset.size()) * sizeof(RecordId);
        return emptySlotsSpace + _hashSetMemUsage + _roaring.getApproximateSize();
    }

private:
    ExpressionContext* _expCtx;

    absl::flat_hash_set<RecordId, RecordId::Hasher> _hashset;
    HashRoaringSet _roaring;
    // Keep the record with id null separately because storage does not
    // consider records with id null to be the same.
    bool hasNullRecordId{false};

    std::unique_ptr<SpillTable> _diskStorageString;
    std::unique_ptr<SpillTable> _diskStorageLong;

    uint64_t _hashSetMemUsage = 0;
};
}  // namespace mongo
