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


#include "mongo/db/repl/oplog_applier_batcher.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {
MONGO_FAIL_POINT_DEFINE(skipOplogBatcherWaitForData);
MONGO_FAIL_POINT_DEFINE(oplogBatcherPauseAfterSuccessfulPeek);

OplogApplierBatcher::OplogApplierBatcher(OplogApplier* oplogApplier, OplogBuffer* oplogBuffer)
    : _oplogApplier(oplogApplier), _oplogBuffer(oplogBuffer), _ops() {}
OplogApplierBatcher::~OplogApplierBatcher() {
    invariant(!_thread);
}

OplogApplierBatch OplogApplierBatcher::getNextBatch(Seconds maxWaitTime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
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

    // Prevent oplog application while there is an active replication state transition.
    if (rss::consensus::IntentRegistry::get(cc().getServiceContext()).activeStateTransition()) {
        // Return an empty batch without touching the batch we have queued up.
        return OplogApplierBatch();
    }

    OplogApplierBatch ops = std::move(_ops);
    _ops = OplogApplierBatch();
    _cv.notify_all();
    return ops;
}

void OplogApplierBatcher::startup(StorageInterface* storageInterface) {
    _thread = std::make_unique<stdx::thread>([this, storageInterface] { _run(storageInterface); });
}

void OplogApplierBatcher::shutdown() {
    if (_thread) {
        _thread->join();
        _thread.reset();
    }
}

std::size_t OplogApplierBatcher::getOpCount(const OplogEntry& entry) {
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

StatusWith<OplogApplierBatch> OplogApplierBatcher::getNextApplierBatch(
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

        if (entry.shouldLogAsDDLOperation() && !serverGlobalParams.quiet.load()) {
            LOGV2(7360109,
                  "Processing DDL command oplog entry in OplogApplierBatcher",
                  "oplogEntry"_attr = entry.toBSONForLogging());
        }

        if (!feature_flags::gReduceMajorityWriteLatency.isEnabled()) {
            // Check for oplog version change.
            if (entry.getVersion() != OplogEntry::kOplogVersion) {
                static constexpr char message[] = "Unexpected oplog version";
                LOGV2_FATAL_CONTINUE(8539100,
                                     message,
                                     "expectedVersion"_attr = OplogEntry::kOplogVersion,
                                     "foundVersion"_attr = entry.getVersion(),
                                     "oplogEntry"_attr = redact(entry.toBSONForLogging()));
                return {ErrorCodes::BadValue,
                        str::stream()
                            << message << ", expected oplog version " << OplogEntry::kOplogVersion
                            << ", found version " << entry.getVersion()
                            << ", oplog entry: " << redact(entry.toBSONForLogging())};
            }
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
                return OplogApplierBatch(std::move(ops), batchStats.totalBytes);
            }
        }

        BatchAction action = _getBatchActionForEntry(entry, batchStats);
        switch (action) {
            case BatchAction::kContinueBatch:
                break;
            case BatchAction::kStartNewBatch:
                if (!ops.empty()) {
                    return OplogApplierBatch(std::move(ops), batchStats.totalBytes);
                }
                break;
            case BatchAction::kProcessIndividually:
                if (ops.empty()) {
                    ops.push_back(std::move(entry));
                    _consume(opCtx, _oplogBuffer);
                }
                return OplogApplierBatch(std::move(ops), batchStats.totalBytes);
        }

        // Apply replication batch limits. Avoid returning an empty batch.
        auto opCount = getOpCount(entry);
        auto opBytes = entry.getRawObjSizeBytes();
        if (batchStats.totalOps > 0) {
            if (batchStats.totalOps + opCount > batchLimits.ops ||
                batchStats.totalBytes + opBytes > batchLimits.bytes) {
                return OplogApplierBatch(std::move(ops), batchStats.totalBytes);
            }
        }

        // If we have a forced batch boundary, apply it.
        if (batchStats.totalOps > 0 && !batchLimits.forceBatchBoundaryAfter.isNull() &&
            entry.getOpTime().getTimestamp() > batchLimits.forceBatchBoundaryAfter &&
            ops.back().getOpTime().getTimestamp() <= batchLimits.forceBatchBoundaryAfter) {
            return OplogApplierBatch(std::move(ops), batchStats.totalBytes);
        }

        // Add op to buffer.
        batchStats.totalOps += opCount;
        batchStats.totalBytes += opBytes;
        batchStats.prepareOps += entry.shouldPrepare();
        batchStats.commitOrAbortOps += entry.isPreparedCommit() || entry.isPreparedAbort();
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
            } catch (const ExceptionFor<ErrorCategory::CancellationError>& e) {
                LOGV2(6572300,
                      "Cancelled in oplog batching; returning current partial batch.",
                      "error"_attr = e);
            }
        }
    }

    return OplogApplierBatch(std::move(ops), batchStats.totalBytes);
}

/**
 * Commands, in most cases, must be processed one at a time, however there are some exceptions:
 *
 * 1) When in secondary steady state oplog application mode, a prepareTransaction entry can be
 *    batched with other entries, while a prepared commitTransaction or abortTransaction entry
 *    can only be batched with other prepared commitTransaction or abortTransaction entries.
 * 2) An applyOps entry from batched writes or unprepared transactions will be expanded to CRUD
 *    operation and thus can be safely batched with other CRUD operations in most cases, unless
 *    it refers to the end of a large transaction (> 16MB) or a transaction that contains DDL
 *    commands, which have to be processed individually (see SERVER-45565).
 */
OplogApplierBatcher::BatchAction OplogApplierBatcher::_getBatchActionForEntry(
    const OplogEntry& entry, const BatchStats& batchStats) {
    // Used by non-commit and non-abort entries to cut the batch if it already contains any
    // commit or abort entries.
    auto continueOrStartNewBatch = [&] {
        return batchStats.commitOrAbortOps > 0 ? OplogApplierBatcher::BatchAction::kStartNewBatch
                                               : OplogApplierBatcher::BatchAction::kContinueBatch;
    };

    // We split the batch on the new Primary entry to ensure there is not record ID reuse within
    // a batch. It can be batched with any subsequent oplog that is batchable as the only
    // possibility of record ID reuse in between a new Primary start.
    if (entry.isNewPrimaryNoop()) {
        return OplogApplierBatcher::BatchAction::kStartNewBatch;
    }

    if (!entry.isCommand()) {
        return entry.getNss().mustBeAppliedInOwnOplogBatch()
            ? OplogApplierBatcher::BatchAction::kProcessIndividually
            : continueOrStartNewBatch();
    }

    if (_oplogApplier->getOptions().mode == OplogApplication::Mode::kSecondary) {
        if (entry.shouldPrepare()) {
            // Grouping too many prepare ops in a batch may have performance implications,
            // so we break the batch when it contains enough prepare ops.
            return batchStats.prepareOps >= kMaxPrepareOpsPerBatch
                ? OplogApplierBatcher::BatchAction::kStartNewBatch
                : continueOrStartNewBatch();
        }
        if (entry.isPreparedCommitOrAbort()) {
            return batchStats.commitOrAbortOps == 0
                ? OplogApplierBatcher::BatchAction::kStartNewBatch
                : OplogApplierBatcher::BatchAction::kContinueBatch;
        }
    }

    // The DBCheck oplog shouldn't be batched with any preceding oplog to ensure that DBCheck is
    // reading from a consistent snapshot. However, it can be batched with any subsequent oplog that
    // is batchable.
    if (entry.getCommandType() == OplogEntry::CommandType::kDbCheck) {
        return OplogApplierBatcher::BatchAction::kStartNewBatch;
    }

    bool processIndividually = (entry.getCommandType() != OplogEntry::CommandType::kApplyOps) ||
        entry.shouldPrepare() || entry.isSingleOplogEntryTransactionWithCommand() ||
        entry.isEndOfLargeTransaction();

    return processIndividually ? OplogApplierBatcher::BatchAction::kProcessIndividually
                               : continueOrStartNewBatch();
}

/**
 * If secondaryDelaySecs is enabled, this function calculates the most recent timestamp of any oplog
 * entries that can be be returned in a batch.
 */
boost::optional<Date_t> OplogApplierBatcher::_calculateSecondaryDelaySecsLatestTimestamp() {
    auto service = cc().getServiceContext();
    auto replCoord = ReplicationCoordinator::get(service);
    auto secondaryDelaySecs = replCoord->getSecondaryDelaySecs();
    if (secondaryDelaySecs <= Seconds(0)) {
        return {};
    }
    auto fastClockSource = service->getFastClockSource();
    return fastClockSource->now() - secondaryDelaySecs;
}

void OplogApplierBatcher::_consume(OperationContext* opCtx, OplogBuffer* oplogBuffer) {
    // This is just to get the op off the buffer; it's been peeked at and queued for application
    // already.
    // If we failed to get an op off the buffer, this means that shutdown() was called between the
    // consumer's calls to peek() and consume(). shutdown() cleared the buffer so there is nothing
    // for us to consume here. Since our postcondition is already met, it is safe to return
    // successfully.
    BSONObj opToPopAndDiscard;
    invariant(oplogBuffer->tryPop(opCtx, &opToPopAndDiscard) || _oplogApplier->inShutdown(),
              str::stream() << "Oplog Applier in shutdown: " << _oplogApplier->inShutdown());
}

void OplogApplierBatcher::_run(StorageInterface* storageInterface) {
    // The OplogApplierBatcher's thread has its own shutdown sequence triggered by the
    // OplogApplier, so we don't want it to be killed in other ways.
    Client::initThread("ReplBatcher",
                       getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                       Client::noSession(),
                       ClientOperationKillableByStepdown{false});

    BatchLimits batchLimits;

    while (true) {
        // When featureFlagReduceMajorityWriteLatency is enabled, OplogWriter takes care of this.
        if (!feature_flags::gReduceMajorityWriteLatency.isEnabled()) {
            globalFailPointRegistry().find("rsSyncApplyStop")->pauseWhileSet();
            batchLimits.secondaryDelaySecsLatestTimestamp =
                _calculateSecondaryDelaySecsLatestTimestamp();
        }

        // Check the limits once per batch since users can change them at runtime.
        batchLimits.ops = getBatchLimitOplogEntries();

        // Use the OplogBuffer to populate a local OplogBatch. Note that the buffer may be empty.
        OplogApplierBatch ops;
        // The only case we can get an interruption in this try/catch block is during shutdown since
        // this thread is unkillable by stepdown. Since this OplogApplierBatcher has its own way to
        // handle shutdown, we can just swallow the exception.
        int retryAttempts = 0;
        for (;;) {
            try {
                auto opCtx = cc().makeOperationContext();
                ScopedAdmissionPriority<ExecutionAdmissionContext> admissionPriority(
                    opCtx.get(), AdmissionContext::Priority::kExempt);

                // During storage change operations, we may shut down storage under a global lock
                // and wait for any storage-using opCtxs to exit, so we will take the global lock
                // here to block the storage change.
                Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

                // Locks the oplog to check its max size, do this in the UninterruptibleLockGuard.
                batchLimits.bytes = getBatchLimitOplogBytes(opCtx.get(), storageInterface);

                // When this feature flag is enabled, the oplogBatchDelayMillis is handled in
                // OplogWriter.
                auto waitToFillBatch = Milliseconds(
                    feature_flags::gReduceMajorityWriteLatency.isEnabled() ? 0
                                                                           : oplogBatchDelayMillis);
                ops = fassertNoTrace(
                    31004, getNextApplierBatch(opCtx.get(), batchLimits, waitToFillBatch));
                break;
            } catch (const ExceptionFor<ErrorCategory::ShutdownError>& e) {
                LOGV2_DEBUG(6133400,
                            1,
                            "Cancelled getting the global lock in Repl Batcher",
                            "error"_attr = e.toStatus());
                invariant(ops.empty());
                break;
            } catch (const ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>&) {
                ++retryAttempts;
                logAndBackoff(10262303,
                              MONGO_LOGV2_DEFAULT_COMPONENT,
                              logv2::LogSeverity::Debug(1),
                              retryAttempts,
                              "Retrying oplog batcher until we can declare our intent.");
            }
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

        // The applier may be in its 'Draining' state. Determines if the OplogApplierBatcher has
        // finished draining the OplogBuffer and should notify the OplogApplier to signal draining
        // is complete.
        if (ops.empty() && !ops.mustShutdown()) {
            // Store the current term. It's checked in signalApplierDrainComplete() to detect if
            // the node has stepped down and stepped back up again. See the declaration of
            // signalApplierDrainComplete() for more details.
            auto replCoord = ReplicationCoordinator::get(cc().getServiceContext());
            auto termWhenExhausted = replCoord->getTerm();
            auto syncState = replCoord->getOplogSyncState();

            // Draining state guarantees the producer has already been fully stopped and no more
            // operations will be pushed in to the oplog buffer until the OplogSyncState changes.
            auto isDraining = syncState == ReplicationCoordinator::OplogSyncState::ApplierDraining;

            // Check the oplog buffer after the applier state to ensure the producer is stopped.
            if (isDraining && _oplogBuffer->isEmpty()) {
                ops.setTermWhenExhausted(termWhenExhausted);
                LOGV2(21239,
                      "Oplog applier buffer has been drained",
                      "term"_attr = termWhenExhausted);
            } else {
                // Don't emit empty batches.
                continue;
            }
        }

        stdx::unique_lock<stdx::mutex> lk(_mutex);
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
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    auto oplogMaxSizeResult = storageInterface->getOplogMaxSize(opCtx);
    auto oplogMaxSize = fassert(40301, oplogMaxSizeResult);
    return std::min(oplogMaxSize / 10, std::size_t(replBatchLimitBytes.load()));
}

}  // namespace repl
}  // namespace mongo
