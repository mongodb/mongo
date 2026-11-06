// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/replicated_fast_count/size_count_checkpoint_flusher.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failDuringFlush);
MONGO_FAIL_POINT_DEFINE(hangBeforePersistingNewFastCountEntries);

const BSONObj kWatermarkMsgObj = BSON("msg" << kWatermarkMsg);
}  // namespace
}  // namespace mongo

namespace mongo::replicated_fast_count::flusher {
namespace {

// Absolute replicated-metadata totals ready to persist, rather than incremental deltas.
struct ReplicatedMetadataCheckpointSnapshot {
    ReplicatedMetadataDeltas updatedCollections;
    Timestamp validAsOf;
};

/**
 * Converts incremental deltas plus a known visible cutoff into a persistable snapshot by reading
 * the currently persisted totals and incrementing them in-place.
 */
ReplicatedMetadataCheckpointSnapshot materializeCheckpointSnapshot(
    OperationContext* opCtx,
    const SizeCountStore& sizeCountStore,
    OplogScanResult scanResult,
    Timestamp scanStartAfterTS) {
    sizeCountStore.readAndIncrementReplicatedMetadata(opCtx, scanResult.deltas);
    return {.updatedCollections = std::move(scanResult.deltas),
            .validAsOf = scanResult.lastTimestamp.value_or(scanStartAfterTS)};
}

/**
 * Persists a materialized checkpoint snapshot to the `sizeCountStore` and updates the valid-as-of
 * timestamp in `timestampStore`.
 *
 * Returns the counts of the writes performed to the `SizeCountStore`.
 */
CheckpointWriteCounts persistCheckpointSnapshot(
    OperationContext* opCtx,
    const ReplicatedMetadataCheckpointSnapshot& checkpoint,
    SizeCountStore& sizeCountStore,
    SizeCountTimestampStore& timestampStore) {
    CheckpointWriteCounts writeCounts{.entryWriteCount = 0, .insertCount = 0, .updateCount = 0};
    for (const auto& [uuid, entry] : checkpoint.updatedCollections) {
        switch (entry.state) {
            case DDLState::kCreated: {
                const boost::optional<SizeCountStore::Entry> oldValue = sizeCountStore.insert(
                    opCtx,
                    uuid,
                    SizeCountStore::Entry{.timestamp = checkpoint.validAsOf,
                                          .size = entry.metadata.sizeCount.size,
                                          .count = entry.metadata.sizeCount.count,
                                          .hash = entry.metadata.hash});
                massert(12299200,
                        fmt::format("Did not expect pre-existing entry for uuid={} in "
                                    "SizeCountStore for collection create",
                                    uuid.toString()),
                        !oldValue.has_value());
                writeCounts.entryWriteCount++;
                writeCounts.insertCount++;
                break;
            }
            case DDLState::kDropped: {
                writeCounts.entryWriteCount += sizeCountStore.remove(opCtx, uuid);
                break;
            }
            case DDLState::kDroppedAndRecreated:
            case DDLState::kNone: {
                const boost::optional<SizeCountStore::Entry> oldValue = sizeCountStore.insert(
                    opCtx,
                    uuid,
                    SizeCountStore::Entry{.timestamp = checkpoint.validAsOf,
                                          .size = entry.metadata.sizeCount.size,
                                          .count = entry.metadata.sizeCount.count,
                                          .hash = entry.metadata.hash},
                    entry.hashUpdateMode);
                if (oldValue.has_value()) {
                    writeCounts.updateCount++;
                } else {
                    writeCounts.insertCount++;
                }
                writeCounts.entryWriteCount++;
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    // Advance the timestamp in `config.fast_count_metadata_store_timestamps` to the new checkpoint
    // timestamp. This moves the global valid-as-of forward so that future checkpoint passes this
    // point.
    timestampStore.insert(opCtx, checkpoint.validAsOf);

    return writeCounts;
}

}  // namespace

void writeWatermark(OperationContext* opCtx) {
    writeConflictRetry(opCtx, "writeWatermark", NamespaceString::kRsOplogNamespace, [&] {
        Lock::GlobalLock writeLock(opCtx, MODE_IX);

        WriteUnitOfWork wuow(opCtx);
        opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(opCtx,
                                                                              kWatermarkMsgObj);
        wuow.commit();
    });
    incrementWatermarksWrittenCount();
}

boost::optional<FlushResult> flush(OperationContext* opCtx,
                                   SizeCountStore& sizeCountStore,
                                   SizeCountTimestampStore& timestampStore,
                                   const OplogScanResult& batch) {
    if (MONGO_unlikely(failDuringFlush.shouldFail())) {
        uasserted(12101802, "Injected failure in flush for testing");
    }

    boost::optional<FlushResult> result;
    size_t flushAttempts = 0;
    writeConflictRetry(opCtx, "flush", NamespaceString::kDefaultOplogCollectionNamespace, [&] {
        if (flushAttempts++ > 0) {
            incrementRetriedFlushCount();
        }
        Lock::GlobalLock writeLock(opCtx, MODE_IX);

        const Timestamp currentValidAsOf = timestampStore.read(opCtx).value_or(Timestamp{});
        const auto checkpoint =
            materializeCheckpointSnapshot(opCtx, sizeCountStore, batch, currentValidAsOf);

        if (currentValidAsOf == checkpoint.validAsOf) {
            tassert(12101801,
                    "Logical size count checkpoint found size count deltas in oplog entries, but "
                    "global valid-as-of did not advance",
                    checkpoint.updatedCollections.empty());
            return;
        }

        hangBeforePersistingNewFastCountEntries.pauseWhileSet();

        WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::atomicGroup);
        const CheckpointWriteCounts writeCounts =
            persistCheckpointSnapshot(opCtx, checkpoint, sizeCountStore, timestampStore);
        wuow.commit();

        result = FlushResult{
            .previousValidAsOfTS = currentValidAsOf,
            .newValidAsOfTS = checkpoint.validAsOf,
            .checkpointBufferSize = batch.deltas.size(),
            .writeCounts = writeCounts,
            .flushAttempts = flushAttempts,
        };
    });

    return result;
}
}  // namespace mongo::replicated_fast_count::flusher
