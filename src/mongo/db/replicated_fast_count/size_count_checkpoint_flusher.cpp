// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/replicated_fast_count/size_count_checkpoint_flusher.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_advance_checkpoint.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace {

MONGO_FAIL_POINT_DEFINE(failDuringFlush);
MONGO_FAIL_POINT_DEFINE(hangBeforePersistingNewFastCountEntries);
}  // namespace

namespace mongo::replicated_fast_count::flusher {

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

        // Source of truth for the last durable checkpoint, replacing batch.startAfter.
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

        WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::nonAtomicGroup);
        const size_t entryWriteCount =
            persistCheckpointSnapshot(opCtx, checkpoint, sizeCountStore, timestampStore);
        wuow.commit();

        result = FlushResult{
            .previousValidAsOfTS = currentValidAsOf,
            .newValidAsOfTS = checkpoint.validAsOf,
            .checkpointBufferSize = batch.deltas.size(),
            .entryWriteCount = entryWriteCount,
            .flushAttempts = flushAttempts,
        };
    });

    return result;
}
}  // namespace mongo::replicated_fast_count::flusher
