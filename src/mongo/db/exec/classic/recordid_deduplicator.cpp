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

#include "mongo/db/exec/classic/recordid_deduplicator.h"

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/pipeline/spilling/spill_table_batch_writer.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"

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
                        [&](const char* str, int size) { _hashset.insert(recordId); });

    return true;
}

void RecordIdDeduplicator::freeMemory(const RecordId& recordId) {
    recordId.withFormat([&](RecordId::Null _) { hasNullRecordId = false; },
                        [&](int64_t rid) { _roaring.erase(rid); },
                        [&](const char* str, int size) { _hashset.erase(recordId); });
}

void RecordIdDeduplicator::spill(SpillingStats& stats, uint64_t maximumMemoryUsageBytes) {
    LOGV2_DEBUG(11001900, 2, "RecordIdDeduplicator does not spill.");
}

}  // namespace mongo
