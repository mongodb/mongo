/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_advance_checkpoint.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"

namespace mongo::replicated_fast_count {
namespace {
struct SizeCountCheckpoint {
    // Absolute size and count totals ready to persist, for each collection modified since the
    // last checkpoint.
    SizeCountDeltas updatedCollections;
    Timestamp validAsOf;
};

SizeCountCheckpoint computeNextCheckpoint(OperationContext* opCtx, Timestamp seekAfterTimestamp) {
    // Scan the oplog from `seekAfterTimestamp` and accumulate size and count deltas for every UUID
    // that has written since the last checkpoint.
    auto scanResult = [&]() -> OplogScanResult {
        AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
        const auto& oplogColl = oplogRead.getCollection();
        massert(12088200, "oplog collection not found", oplogColl);

        // By utilizing a forward cursor, the cursor should respect oplog visibility rules and only
        // read up until the no holes point.
        auto oplogCursor = oplogColl->getRecordStore()->getCursor(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx));
        return aggregateSizeCountDeltasInOplog(
            *oplogCursor, seekAfterTimestamp, {}, /*isCheckpoint=*/true);
    }();

    // Combine the oplog deltas with the currently persisted totals to produce the absolute values
    // ready to persist.
    readAndIncrementSizeCounts(opCtx, scanResult.deltas);

    return {std::move(scanResult.deltas), scanResult.lastTimestamp.value_or(seekAfterTimestamp)};
}

void persistCheckpoint(OperationContext* opCtx,
                       const SizeCountCheckpoint& checkpoint,
                       SizeCountStore& sizeCountStore,
                       SizeCountTimestampStore& timestampStore) {
    for (const auto& [uuid, entry] : checkpoint.updatedCollections) {
        switch (entry.state) {
            case DDLState::kCreated: {
                sizeCountStore.insert(opCtx,
                                      uuid,
                                      SizeCountStore::Entry{.timestamp = checkpoint.validAsOf,
                                                            .size = entry.sizeCount.size,
                                                            .count = entry.sizeCount.count});
                break;
            }
            case DDLState::kDropped: {
                sizeCountStore.remove(opCtx, uuid);
                break;
            }
            case DDLState::kNone: {
                sizeCountStore.write(opCtx,
                                     uuid,
                                     SizeCountStore::Entry{.timestamp = checkpoint.validAsOf,
                                                           .size = entry.sizeCount.size,
                                                           .count = entry.sizeCount.count});
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    // Advance the timestamp in `config.fast_count_metadata_store_timestamps` to the new checkpoint
    // timestamp. This moves the global valid-as-of forward so that future checkpoint passes this
    // point.
    timestampStore.write(opCtx, checkpoint.validAsOf);
}

}  // namespace

void advanceCheckpoint(OperationContext* opCtx,
                       SizeCountStore& sizeCountStore,
                       SizeCountTimestampStore& timestampStore) {
    const auto currentValidAsOf = timestampStore.read(opCtx).value_or(Timestamp::min());
    const auto checkpoint = computeNextCheckpoint(opCtx, currentValidAsOf);
    if (currentValidAsOf == checkpoint.validAsOf) {
        // No new oplog entries in this checkpoint; so there's no work to be done.
        massert(12088201,
                "Logical size count checkpoint found size count deltas in oplog entries, but "
                "global valid-as-of did not advance",
                checkpoint.updatedCollections.empty());
        return;
    }

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
    persistCheckpoint(opCtx, checkpoint, sizeCountStore, timestampStore);
    wuow.commit();
}

}  // namespace mongo::replicated_fast_count
