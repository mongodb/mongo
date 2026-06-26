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
#include "mongo/db/replicated_fast_count/size_count_checkpoint_flusher.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_advance_checkpoint.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace {

MONGO_FAIL_POINT_DEFINE(failDuringFlush);
MONGO_FAIL_POINT_DEFINE(hangAfterReplicatedFastCountSnapshot);
MONGO_FAIL_POINT_DEFINE(hangBeforePersistingNewFastCountEntries);
MONGO_FAIL_POINT_DEFINE(sleepAfterFlush);
}  // namespace

namespace mongo::replicated_fast_count {

SizeCountCheckpointFlusher::SizeCountCheckpointFlusher(SizeCountStore* sizeCountStore,
                                                       SizeCountTimestampStore* timestampStore)
    : _sizeCountStore(sizeCountStore), _timestampStore(timestampStore) {}

void SizeCountCheckpointFlusher::run(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer) {
    ON_BLOCK_EXIT([&] {
        std::lock_guard lk(_mutex);
        _flushRequested = false;
    });

    while (true) {
        try {
            {
                std::unique_lock lk(_mutex);
                opCtx->waitForConditionOrInterrupt(
                    _flushCv, lk, [this] { return _flushRequested; });
                _flushRequested = false;
            }
            _runOneFlushCycle(opCtx, buffer);
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::InterruptedDueToReplStateChange ||
                ex.code() == ErrorCodes::NotWritablePrimary) {
                LOGV2_DEBUG(12917804,
                            2,
                            "SizeCountCheckpointFlusher interrupted due to replication state",
                            "error"_attr = ex.toStatus());
                return;
            } else {
                incrementFlushFailureCount();
                LOGV2_WARNING(12917805,
                              "Exception handled in SizeCountCheckpointFlusher::run()",
                              "error"_attr = ex.toStatus());
            }
        }
    }
}

void SizeCountCheckpointFlusher::requestFlush() {
    {
        std::lock_guard lk(_mutex);
        _flushRequested = true;
    }
    _flushCv.notify_one();
}

bool SizeCountCheckpointFlusher::isFlushRequested_ForTest() const {
    std::lock_guard lk(_mutex);
    return _flushRequested;
}

void SizeCountCheckpointFlusher::runOneFlushCycle_ForTest(OperationContext* opCtx,
                                                          SizeCountCheckpointBuffer& buffer) {
    _runOneFlushCycle(opCtx, buffer);
}

void SizeCountCheckpointFlusher::_runOneFlushCycle(OperationContext* opCtx,
                                                   SizeCountCheckpointBuffer& buffer) {
    const Date_t flushStart = Date_t::now();
    const size_t entryWriteCount = _doFlush(opCtx, buffer);

    sleepAfterFlush.execute([](const BSONObj& data) {
        if (auto elem = data["sleepMs"]; elem) {
            sleepmillis(elem.numberInt());
        }
    });

    recordFlush(flushStart, entryWriteCount);
}

size_t SizeCountCheckpointFlusher::_doFlush(OperationContext* opCtx,
                                            SizeCountCheckpointBuffer& buffer) {
    if (MONGO_unlikely(failDuringFlush.shouldFail())) {
        uasserted(12101802, "Injected failure in _runOneFlushCycle for testing");
    }

    auto batch = buffer.checkoutForFlush();
    hangAfterReplicatedFastCountSnapshot.pauseWhileSet();

    if (!batch) {
        // No batch to flush, 0 flushed batch size.
        return 0;
    }

    size_t entryWriteCount = 0;
    writeConflictRetry(opCtx, "flush", NamespaceString::kDefaultOplogCollectionNamespace, [&] {
        Lock::GlobalLock writeLock(opCtx, MODE_IX);

        // Source of truth for the last durable checkpoint, replacing batch.startAfter.
        const Timestamp currentValidAsOf = _timestampStore->read(opCtx).value_or(Timestamp{});
        const auto checkpoint =
            materializeCheckpointSnapshot(opCtx, *_sizeCountStore, *batch, currentValidAsOf);

        if (currentValidAsOf == checkpoint.validAsOf) {
            tassert(12101801,
                    "Logical size count checkpoint found size count deltas in oplog "
                    "entries, but "
                    "global valid-as-of did not advance",
                    checkpoint.updatedCollections.empty());
            return;
        }

        hangBeforePersistingNewFastCountEntries.pauseWhileSet();

        WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        entryWriteCount =
            persistCheckpointSnapshot(opCtx, checkpoint, *_sizeCountStore, *_timestampStore);
        wuow.commit();
    });
    buffer.acknowledgeFlushSuccess();

    return entryWriteCount;
}

}  // namespace mongo::replicated_fast_count
