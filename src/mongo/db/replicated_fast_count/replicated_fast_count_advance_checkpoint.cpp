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
#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"

namespace mongo::replicated_fast_count {
namespace {

SizeCountCheckpointSnapshot computeNextCheckpoint(OperationContext* opCtx,
                                                  const SizeCountStore& sizeCountStore,
                                                  Timestamp seekAfterTimestamp) {
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
        return aggregateSizeCountDeltasInOplog(*oplogCursor,
                                               seekAfterTimestamp,
                                               {},
                                               /*isCheckpoint=*/true,
                                               oplogColl->uuid());
    }();

    return materializeCheckpointSnapshot(
        opCtx, sizeCountStore, std::move(scanResult), std::move(seekAfterTimestamp));
}
}  // namespace

SizeCountCheckpointSnapshot materializeCheckpointSnapshot(OperationContext* opCtx,
                                                          const SizeCountStore& sizeCountStore,
                                                          OplogScanResult scanResult,
                                                          Timestamp scanStartAfterTS) {
    sizeCountStore.readAndIncrementSizeCounts(opCtx, scanResult.deltas);
    return {.updatedCollections = std::move(scanResult.deltas),
            .validAsOf = scanResult.lastTimestamp.value_or(scanStartAfterTS)};
}

size_t persistCheckpointSnapshot(OperationContext* opCtx,
                                 const SizeCountCheckpointSnapshot& checkpoint,
                                 SizeCountStore& sizeCountStore,
                                 SizeCountTimestampStore& timestampStore) {
    size_t entryWriteCount = 0;
    for (const auto& [uuid, entry] : checkpoint.updatedCollections) {
        switch (entry.state) {
            case DDLState::kCreated: {
                sizeCountStore.insert(opCtx,
                                      uuid,
                                      SizeCountStore::Entry{.timestamp = checkpoint.validAsOf,
                                                            .size = entry.sizeCount.size,
                                                            .count = entry.sizeCount.count});
                entryWriteCount++;
                break;
            }
            case DDLState::kDropped: {
                entryWriteCount += sizeCountStore.remove(opCtx, uuid);
                break;
            }
            case DDLState::kDroppedAndRecreated:
            case DDLState::kNone: {
                sizeCountStore.write(opCtx,
                                     uuid,
                                     SizeCountStore::Entry{.timestamp = checkpoint.validAsOf,
                                                           .size = entry.sizeCount.size,
                                                           .count = entry.sizeCount.count});
                entryWriteCount++;
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

    return entryWriteCount;
}

size_t advanceCheckpoint(OperationContext* opCtx,
                         SizeCountStore& sizeCountStore,
                         SizeCountTimestampStore& timestampStore) {
    // A global IS lock cannot be upgraded to an IX lock, so we take the IX lock up front to avoid
    // releasing locks during execution of this function.
    Lock::GlobalLock writeLock(opCtx, MODE_IX);

    const Timestamp currentValidAsOf = timestampStore.read(opCtx).value_or(Timestamp::min());
    const auto checkpoint = computeNextCheckpoint(opCtx, sizeCountStore, currentValidAsOf);
    if (currentValidAsOf == checkpoint.validAsOf) {
        // No new oplog entries in this checkpoint; so there's no work to be done.
        massert(12088201,
                "Logical size count checkpoint found size count deltas in oplog entries, but "
                "global valid-as-of did not advance",
                checkpoint.updatedCollections.empty());
        return 0;
    }

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
    const size_t entryWriteCount =
        persistCheckpointSnapshot(opCtx, checkpoint, sizeCountStore, timestampStore);
    wuow.commit();

    return entryWriteCount;
}

}  // namespace mongo::replicated_fast_count
