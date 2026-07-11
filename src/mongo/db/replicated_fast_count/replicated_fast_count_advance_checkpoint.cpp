// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
                                               oplogColl->uuid(),
                                               /*isCheckpoint=*/true);
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
