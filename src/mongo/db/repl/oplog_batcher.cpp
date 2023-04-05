/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/db/repl/oplog_batcher.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {
MONGO_FAIL_POINT_DEFINE(skipOplogBatcherWaitForData);
MONGO_FAIL_POINT_DEFINE(oplogBatcherPauseAfterSuccessfulPeek);

OplogBatcher::OplogBatcher(OplogApplier* oplogApplier, OplogBuffer* oplogBuffer)
    : _oplogApplier(oplogApplier), _oplogBuffer(oplogBuffer), _ops(0) {}
OplogBatcher::~OplogBatcher() {
    invariant(!_thread);
}

OplogBatch OplogBatcher::getNextBatch(Seconds maxWaitTime) {
    stdx::unique_lock<Latch> lk(_mutex);
    // _ops can indicate the following cases:
    // 1. A new batch is ready to consume.
    // 2. Shutdown.
    // 3. The batch has (or had) exhausted the buffer in draining mode.
    // 4. Empty batch since the batch has/had exhausted the buffer but not in draining mode,
    //    so there could be new oplog entries coming.
    // 5. Empty batch since the batcher is still running.
    //
    // In case (4) and (5), we wait for up to "maxWaitTime".
    if (_ops.empty() && !_ops.mustShutdown() && !_ops.termWhenExhausted()) {
        // We intentionally don't care about whether this returns due to signaling or timeout
        // since we do the same thing either way: return whatever is in _ops.
        (void)_cv.wait_for(lk, maxWaitTime.toSystemDuration());
    }

    OplogBatch ops = std::move(_ops);
    _ops = OplogBatch(0);
    _cv.notify_all();
    return ops;
}

void OplogBatcher::startup(StorageInterface* storageInterface) {
    _thread = std::make_unique<stdx::thread>([this, storageInterface] { _run(storageInterface); });
}

void OplogBatcher::shutdown() {
    if (_thread) {
        _thread->join();
        _thread.reset();
    }
}

std::size_t OplogBatcher::getOpCount(const OplogEntry& entry) {
    // Get the number of operations enclosed in 'applyOps'. The 'count' field only exists in
    // the last applyOps oplog entry of a large transaction that has multiple oplog entries,
    // and when not present, we fallback to get the count by using BSONObj::nFields() which
    // could be slower.
    if (entry.isTerminalApplyOps() || entry.shouldPrepare()) {
        auto count = entry.getObject().getIntField(ApplyOpsCommandInfoBase::kCountFieldName);
        if (count > 0) {
            return std::size_t(count);
        }
        auto size =
            entry.getObject()[ApplyOpsCommandInfoBase::kOperationsFieldName].Obj().nFields();
        return size > 0 ? std::size_t(size) : 1U;
    }

    return 1U;
}

StatusWith<std::vector<OplogEntry>> OplogBatcher::getNextApplierBatch(
    OperationContext* opCtx, const BatchLimits& batchLimits, Milliseconds waitToFillBatch) {
    if (batchLimits.ops == 0) {
        return Status(ErrorCodes::InvalidOptions, "Batch size must be greater than 0.");
    }

    BatchStats batchStats;
    std::vector<OplogEntry> ops;
    BSONObj op;
    Date_t batchDeadline;
    if (waitToFillBatch > Milliseconds(0)) {
        batchDeadline =
            opCtx->getServiceContext()->getPreciseClockSource()->now() + waitToFillBatch;
    }
    while (_oplogBuffer->peek(opCtx, &op)) {
        oplogBatcherPauseAfterSuccessfulPeek.pauseWhileSet();
        auto entry = OplogEntry(op);

        if (entry.shouldLogAsDDLOperation()) {
            LOGV2(7360109,
                  "Processing DDL command oplog entry in OplogBatcher",
                  "oplogEntry"_attr = entry.toBSONForLogging());
        }

        // Check for oplog version change.
        if (entry.getVersion() != OplogEntry::kOplogVersion) {
            static constexpr char message[] = "Unexpected oplog version";
            LOGV2_FATAL_CONTINUE(21240,
                                 message,
                                 "expectedVersion"_attr = OplogEntry::kOplogVersion,
                                 "foundVersion"_attr = entry.getVersion(),
                                 "oplogEntry"_attr = redact(entry.toBSONForLogging()));
            return {ErrorCodes::BadValue,
                    str::stream() << message << ", expected oplog version "
                                  << OplogEntry::kOplogVersion << ", found version "
                                  << entry.getVersion()
                                  << ", oplog entry: " << redact(entry.toBSONForLogging())};
        }

        if (batchLimits.secondaryDelaySecsLatestTimestamp) {
            auto entryTime =
                Date_t::fromDurationSinceEpoch(Seconds(entry.getTimestamp().getSecs()));
            if (entryTime > *batchLimits.secondaryDelaySecsLatestTimestamp) {
                if (ops.empty()) {
                    // Sleep if we've got nothing to do. Only sleep for 1 second at a time to allow
                    // reconfigs and shutdown to occur.
                    sleepsecs(1);
                }
                return std::move(ops);
            }
        }

        BatchAction action = _getBatchActionForEntry(entry, batchStats);
        switch (action) {
            case BatchAction::kContinueBatch:
                break;
            case BatchAction::kStartNewBatch:
                if (!ops.empty()) {
                    return std::move(ops);
                }
                break;
            case BatchAction::kProcessIndividually:
                if (ops.empty()) {
                    ops.push_back(std::move(entry));
                    _consume(opCtx, _oplogBuffer);
                }
                return std::move(ops);
        }

        // Apply replication batch limits. Avoid returning an empty batch.
        auto opCount = getOpCount(entry);
        auto opBytes = entry.getRawObjSizeBytes();
        if (batchStats.totalOps > 0) {
            if (batchStats.totalOps + opCount > batchLimits.ops ||
                batchStats.totalBytes + opBytes > batchLimits.bytes) {
                return std::move(ops);
            }
        }

        // If we have a forced batch boundary, apply it.
        if (batchStats.totalOps > 0 && !batchLimits.forceBatchBoundaryAfter.isNull() &&
            entry.getOpTime().getTimestamp() > batchLimits.forceBatchBoundaryAfter &&
            ops.back().getOpTime().getTimestamp() <= batchLimits.forceBatchBoundaryAfter) {
            return std::move(ops);
        }

        // Add op to buffer.
        batchStats.totalOps += opCount;
        batchStats.totalBytes += opBytes;
        batchStats.prepareOps += entry.shouldPrepare();
        ops.push_back(std::move(entry));
        _consume(opCtx, _oplogBuffer);

        // At this point we either have a partial batch or an exactly full batch; if we are using
        // a wait to fill the batch, we should wait if and only if the batch is partial.
        if (batchDeadline != Date_t() && batchStats.totalOps < batchLimits.ops &&
            batchStats.totalBytes < batchLimits.bytes) {
            LOGV2_DEBUG(6572301,
                        3,
                        "Waiting for batch to fill",
                        "deadline"_attr = batchDeadline,
                        "waitToFillBatch"_attr = waitToFillBatch,
                        "totalOps"_attr = batchStats.totalOps,
                        "totalBytes"_attr = batchStats.totalBytes);
            try {
                _oplogBuffer->waitForDataUntil(batchDeadline, opCtx);
            } catch (const ExceptionForCat<ErrorCategory::CancellationError>& e) {
                LOGV2(6572300,
                      "Cancelled in oplog batching; returning current partial batch.",
                      "error"_attr = e);
            }
        }
    }
    return std::move(ops);
}

/**
 * Commands, in most cases, must be processed one at a time, however there are some exceptions:
 *
 * 1) When in secondary steady state oplog application mode, a prepareTransaction entry can be
 *    batched with other entries, while a prepared commitTransaction or abortTransaction entry
 *    is always processed individually in its own batch.
 * 2) An applyOps entry from batched writes or unprepared transactions will be expanded to CRUD
 *    operation and thus can be safely batched with other CRUD operations in most cases, unless
 *    it refers to the end of a large transaction (> 16MB) or a transaction that contains DDL
 *    commands, which have to be processed individually (see SERVER-45565).
 */
OplogBatcher::BatchAction OplogBatcher::_getBatchActionForEntry(const OplogEntry& entry,
                                                                const BatchStats& batchStats) {
    if (!entry.isCommand()) {
        return entry.getNss().mustBeAppliedInOwnOplogBatch()
            ? OplogBatcher::BatchAction::kProcessIndividually
            : OplogBatcher::BatchAction::kContinueBatch;
    }

    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (repl::feature_flags::gApplyPreparedTxnsInParallel.isEnabledAndIgnoreFCVUnsafe() &&
        _oplogApplier->getOptions().mode == OplogApplication::Mode::kSecondary) {
        if (entry.shouldPrepare()) {
            // Grouping too many prepare ops in a batch may have performance implications,
            // so we break the batch when it contains enough prepare ops.
            return batchStats.prepareOps >= kMaxPrepareOpsPerBatch
                ? OplogBatcher::BatchAction::kStartNewBatch
                : OplogBatcher::BatchAction::kContinueBatch;
        }
        if (entry.isPreparedCommitOrAbort()) {
            return OplogBatcher::BatchAction::kProcessIndividually;
        }
    }

    bool processIndividually = (entry.getCommandType() != OplogEntry::CommandType::kApplyOps) ||
        entry.shouldPrepare() || entry.isSingleOplogEntryTransactionWithCommand() ||
        entry.isEndOfLargeTransaction();

    return processIndividually ? OplogBatcher::BatchAction::kProcessIndividually
                               : OplogBatcher::BatchAction::kContinueBatch;
}

/**
 * If secondaryDelaySecs is enabled, this function calculates the most recent timestamp of any oplog
 * entries that can be be returned in a batch.
 */
boost::optional<Date_t> OplogBatcher::_calculateSecondaryDelaySecsLatestTimestamp() {
    auto service = cc().getServiceContext();
    auto replCoord = ReplicationCoordinator::get(service);
    auto secondaryDelaySecs = replCoord->getSecondaryDelaySecs();
    if (secondaryDelaySecs <= Seconds(0)) {
        return {};
    }
    auto fastClockSource = service->getFastClockSource();
    return fastClockSource->now() - secondaryDelaySecs;
}

void OplogBatcher::_consume(OperationContext* opCtx, OplogBuffer* oplogBuffer) {
    // This is just to get the op off the buffer; it's been peeked at and queued for application
    // already.
    // If we failed to get an op off the buffer, this means that shutdown() was called between the
    // consumer's calls to peek() and consume(). shutdown() cleared the buffer so there is nothing
    // for us to consume here. Since our postcondition is already met, it is safe to return
    // successfully.
    BSONObj opToPopAndDiscard;
    invariant(oplogBuffer->tryPop(opCtx, &opToPopAndDiscard) || _oplogApplier->inShutdown());
}

void OplogBatcher::_run(StorageInterface* storageInterface) {
    Client::initThread("ReplBatcher");

    BatchLimits batchLimits;

    while (true) {
        globalFailPointRegistry().find("rsSyncApplyStop")->pauseWhileSet();

        batchLimits.secondaryDelaySecsLatestTimestamp =
            _calculateSecondaryDelaySecsLatestTimestamp();

        // Check the limits once per batch since users can change them at runtime.
        batchLimits.ops = getBatchLimitOplogEntries();

        // Use the OplogBuffer to populate a local OplogBatch. Note that the buffer may be empty.
        OplogBatch ops(batchLimits.ops);
        try {
            auto opCtx = cc().makeOperationContext();
            // We do not want to serialize the OplogBatcher with oplog application, nor
            // do we want to take a WiredTiger read ticket.
            ShouldNotConflictWithSecondaryBatchApplicationBlock noConflict(opCtx->lockState());
            opCtx->lockState()->setAdmissionPriority(AdmissionContext::Priority::kImmediate);

            // During storage change operations, we may shut down storage under a global lock
            // and wait for any storage-using opCtxs to exit.  This results in a deadlock with
            // uninterruptible global locks, so we will take the global lock here to block the
            // storage change.  The rest of the batch acquisition remains uninterruptible.
            Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

            // This use of UninterruptibleLockGuard is intentional. It is undesirable to use an
            // UninterruptibleLockGuard in client operations because stepdown requires the
            // ability to interrupt client operations. However, it is acceptable to use an
            // UninterruptibleLockGuard in batch application because the only cause of
            // interruption would be shutdown, and the ReplBatcher thread has its own shutdown
            // handling.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());  // NOLINT.

            // Locks the oplog to check its max size, do this in the UninterruptibleLockGuard.
            batchLimits.bytes = getBatchLimitOplogBytes(opCtx.get(), storageInterface);

            auto oplogEntries = fassertNoTrace(
                31004,
                getNextApplierBatch(opCtx.get(), batchLimits, Milliseconds(oplogBatchDelayMillis)));
            for (const auto& oplogEntry : oplogEntries) {
                ops.emplace_back(oplogEntry);
            }
        } catch (const ExceptionForCat<ErrorCategory::CancellationError>& e) {
            LOGV2_DEBUG(6133400,
                        1,
                        "Cancelled getting the global lock in Repl Batcher",
                        "error"_attr = e.toStatus());
            invariant(ops.empty());
        }

        // If we don't have anything in the batch, wait a bit for something to appear.
        if (ops.empty()) {
            if (_oplogApplier->inShutdown()) {
                ops.setMustShutdownFlag();
            } else {
                // Block up to 1 second. Skip waiting if the failpoint is enabled.
                if (MONGO_unlikely(skipOplogBatcherWaitForData.shouldFail())) {
                    // do no waiting.
                } else {
                    _oplogBuffer->waitForData(Seconds(1));
                }
            }
        }

        // The applier may be in its 'Draining' state. Determines if the OplogBatcher has finished
        // draining the OplogBuffer and should notify the OplogApplier to signal draining is
        // complete.
        if (ops.empty() && !ops.mustShutdown()) {
            // Store the current term. It's checked in signalDrainComplete() to detect if the node
            // has stepped down and stepped back up again. See the declaration of
            // signalDrainComplete() for more details.
            auto replCoord = ReplicationCoordinator::get(cc().getServiceContext());
            auto termWhenBufferIsEmpty = replCoord->getTerm();

            // Draining state guarantees the producer has already been fully stopped and no more
            // operations will be pushed in to the oplog buffer until the applier state changes.
            auto isDraining =
                replCoord->getApplierState() == ReplicationCoordinator::ApplierState::Draining ||
                replCoord->getApplierState() ==
                    ReplicationCoordinator::ApplierState::DrainingForShardSplit;

            // Check the oplog buffer after the applier state to ensure the producer is stopped.
            if (isDraining && _oplogBuffer->isEmpty()) {
                ops.setTermWhenExhausted(termWhenBufferIsEmpty);
                LOGV2(21239,
                      "Oplog buffer has been drained in term {term}",
                      "Oplog buffer has been drained",
                      "term"_attr = termWhenBufferIsEmpty);
            } else {
                // Don't emit empty batches.
                continue;
            }
        }

        stdx::unique_lock<Latch> lk(_mutex);
        // Block until the previous batch has been taken.
        _cv.wait(lk, [&] { return _ops.empty() && !_ops.termWhenExhausted(); });
        _ops = std::move(ops);
        _cv.notify_all();
        if (_ops.mustShutdown()) {
            return;
        }
    }
}

std::size_t getBatchLimitOplogEntries() {
    return std::size_t(replBatchLimitOperations.load());
}

std::size_t getBatchLimitOplogBytes(OperationContext* opCtx, StorageInterface* storageInterface) {
    // We can't change the timestamp source within a write unit of work.
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    auto oplogMaxSizeResult = storageInterface->getOplogMaxSize(opCtx);
    auto oplogMaxSize = fassert(40301, oplogMaxSizeResult);
    return std::min(oplogMaxSize / 10, std::size_t(replBatchLimitBytes.load()));
}

}  // namespace repl
}  // namespace mongo
