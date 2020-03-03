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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/db/repl/oplog_batcher.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace repl {
MONGO_FAIL_POINT_DEFINE(skipOplogBatcherWaitForData);

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

namespace {
/**
 * Returns whether an oplog entry represents an implicit commit for a transaction which has not
 * been prepared.  An entry is an unprepared commit if it has a boolean "prepared" field set to
 * false and "isPartial" is not present.
 */
bool isUnpreparedCommit(const OplogEntry& entry) {
    if (entry.getCommandType() != OplogEntry::CommandType::kApplyOps) {
        return false;
    }

    if (entry.isPartialTransaction()) {
        return false;
    }

    if (entry.shouldPrepare()) {
        return false;
    }

    return true;
}

/**
 * Returns true if this oplog entry must be processed in its own batch and cannot be grouped with
 * other entries.
 *
 * Commands, in most cases, must be processed one at a time. The exceptions to this rule are
 * unprepared applyOps and unprepared commitTransaction for transactions that only contain CRUD
 * operations and commands found within large transactions (>16MB). The prior two cases expand to
 * CRUD operations, which can be safely batched with other CRUD operations. All other command oplog
 * entries, including unprepared applyOps/commitTransaction for transactions that contain commands,
 * must be processed in their own batch.
 * Note that 'unprepared applyOps' could mean a partial transaction oplog entry, an implicit commit
 * applyOps oplog entry, or an atomic applyOps oplog entry outside of a transaction.
 *
 * Command operations inside large transactions do not need to be processed individually as long as
 * the final oplog entry in the transaction is processed individually, since the operations are not
 * actually run until the commit operation is reached.
 *
 * Oplog entries on 'system.views' should also be processed one at a time. View catalog immediately
 * reflects changes for each oplog entry so we can see inconsistent view catalog if multiple oplog
 * entries on 'system.views' are being applied out of the original order.
 *
 * Process updates to 'admin.system.version' individually as well so the secondary's FCV when
 * processing each operation matches the primary's when committing that operation.
 *
 * The ends of large transactions (> 16MB) should also be processed immediately on its own in order
 * to avoid scenarios where parts of the transaction is batched with other operations not in the
 * transaction.
 */
bool mustProcessIndividually(const OplogEntry& entry) {
    if (entry.isCommand()) {
        if (entry.getCommandType() != OplogEntry::CommandType::kApplyOps || entry.shouldPrepare() ||
            entry.isSingleOplogEntryTransactionWithCommand() || entry.isEndOfLargeTransaction()) {
            return true;
        } else {
            // This branch covers unprepared CRUD applyOps and unprepared CRUD commits.
            return false;
        }
    } else if (entry.getNss().isSystemDotViews()) {
        return true;
    } else if (entry.getNss().isServerConfigurationCollection()) {
        return true;
    }
    return false;
}

/**
 * Returns the number of logical operations represented by an oplog entry.
 * This is usually one but may be greater than one in certain cases, such as in a commitTransaction
 * command.
 */
std::size_t getOpCount(const OplogEntry& entry) {
    if (isUnpreparedCommit(entry)) {
        auto count = entry.getObject().getIntField(CommitTransactionOplogObject::kCountFieldName);
        if (count > 0) {
            return std::size_t(count);
        }
    }
    return 1U;
}
}  // namespace

StatusWith<std::vector<OplogEntry>> OplogBatcher::getNextApplierBatch(
    OperationContext* opCtx, const BatchLimits& batchLimits) {
    if (batchLimits.ops == 0) {
        return Status(ErrorCodes::InvalidOptions, "Batch size must be greater than 0.");
    }

    std::size_t totalOps = 0;
    std::uint32_t totalBytes = 0;
    std::vector<OplogEntry> ops;
    BSONObj op;
    while (_oplogBuffer->peek(opCtx, &op)) {
        auto entry = OplogEntry(op);

        // Check for oplog version change.
        if (entry.getVersion() != OplogEntry::kOplogVersion) {
            std::string message = str::stream()
                << "expected oplog version " << OplogEntry::kOplogVersion << " but found version "
                << entry.getVersion() << " in oplog entry: " << redact(entry.toBSON());
            LOGV2_FATAL(21240, "{message}", "message"_attr = message);
            return {ErrorCodes::BadValue, message};
        }

        if (batchLimits.slaveDelayLatestTimestamp) {
            auto entryTime =
                Date_t::fromDurationSinceEpoch(Seconds(entry.getTimestamp().getSecs()));
            if (entryTime > *batchLimits.slaveDelayLatestTimestamp) {
                if (ops.empty()) {
                    // Sleep if we've got nothing to do. Only sleep for 1 second at a time to allow
                    // reconfigs and shutdown to occur.
                    sleepsecs(1);
                }
                return std::move(ops);
            }
        }

        if (mustProcessIndividually(entry)) {
            if (ops.empty()) {
                ops.push_back(std::move(entry));
                _consume(opCtx, _oplogBuffer);
            }

            // Otherwise, apply what we have so far and come back for this entry.
            return std::move(ops);
        }

        // Apply replication batch limits. Avoid returning an empty batch.
        auto opCount = getOpCount(entry);
        auto opBytes = entry.getRawObjSizeBytes();
        if (totalOps > 0) {
            if (totalOps + opCount > batchLimits.ops || totalBytes + opBytes > batchLimits.bytes) {
                return std::move(ops);
            }
        }

        // If we have a forced batch boundary, apply it.
        if (totalOps > 0 && !batchLimits.forceBatchBoundaryAfter.isNull() &&
            entry.getOpTime().getTimestamp() > batchLimits.forceBatchBoundaryAfter &&
            ops.back().getOpTime().getTimestamp() <= batchLimits.forceBatchBoundaryAfter) {
            return std::move(ops);
        }

        // Add op to buffer.
        totalOps += opCount;
        totalBytes += opBytes;
        ops.push_back(std::move(entry));
        _consume(opCtx, _oplogBuffer);
    }
    return std::move(ops);
}

/**
 * If slaveDelay is enabled, this function calculates the most recent timestamp of any oplog
 * entries that can be be returned in a batch.
 */
boost::optional<Date_t> OplogBatcher::_calculateSlaveDelayLatestTimestamp() {
    auto service = cc().getServiceContext();
    auto replCoord = ReplicationCoordinator::get(service);
    auto slaveDelay = replCoord->getSlaveDelaySecs();
    if (slaveDelay <= Seconds(0)) {
        return {};
    }
    auto fastClockSource = service->getFastClockSource();
    return fastClockSource->now() - slaveDelay;
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

        batchLimits.slaveDelayLatestTimestamp = _calculateSlaveDelayLatestTimestamp();

        // Check the limits once per batch since users can change them at runtime.
        batchLimits.ops = getBatchLimitOplogEntries();

        // Use the OplogBuffer to populate a local OplogBatch. Note that the buffer may be empty.
        OplogBatch ops(batchLimits.ops);
        {
            auto opCtx = cc().makeOperationContext();

            // This use of UninterruptibleLockGuard is intentional. It is undesirable to use an
            // UninterruptibleLockGuard in client operations because stepdown requires the
            // ability to interrupt client operations. However, it is acceptable to use an
            // UninterruptibleLockGuard in batch application because the only cause of
            // interruption would be shutdown, and the ReplBatcher thread has its own shutdown
            // handling.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // Locks the oplog to check its max size, do this in the UninterruptibleLockGuard.
            batchLimits.bytes = getBatchLimitOplogBytes(opCtx.get(), storageInterface);

            auto oplogEntries =
                fassertNoTrace(31004, getNextApplierBatch(opCtx.get(), batchLimits));
            for (const auto& oplogEntry : oplogEntries) {
                ops.emplace_back(oplogEntry);
            }

            // If we don't have anything in the batch, wait a bit for something to appear.
            if (oplogEntries.empty()) {
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
                replCoord->getApplierState() == ReplicationCoordinator::ApplierState::Draining;

            // Check the oplog buffer after the applier state to ensure the producer is stopped.
            if (isDraining && _oplogBuffer->isEmpty()) {
                ops.setTermWhenExhausted(termWhenBufferIsEmpty);
                LOGV2(21239,
                      "Oplog buffer has been drained in term {term}",
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
    // We're only reading oplog metadata, so the timestamp is not important.  If we read with the
    // default (which is kLastApplied on secondaries), we may end up with a reader that is at
    // kLastApplied.  If we then roll back, then when we reconstruct prepared transactions during
    // rollback recovery we will be preparing transactions before the read timestamp, which triggers
    // an assertion in WiredTiger.
    ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kNoTimestamp);
    auto oplogMaxSizeResult = storageInterface->getOplogMaxSize(opCtx);
    auto oplogMaxSize = fassert(40301, oplogMaxSizeResult);
    return std::min(oplogMaxSize / 10, std::size_t(replBatchLimitBytes.load()));
}

}  // namespace repl
}  // namespace mongo
