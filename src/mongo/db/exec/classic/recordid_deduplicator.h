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

#pragma once

#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/spilling/spilling_stats.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/util/hash_roaring_set.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/spill_table.h"

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
        return _hashset.size() + _roaring.getApproximateSize();
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
};
}  // namespace mongo
