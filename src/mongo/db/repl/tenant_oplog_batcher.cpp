/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/repl/tenant_oplog_batcher.h"

#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/oplog_batcher.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {
namespace repl {
TenantOplogBatcher::TenantOplogBatcher(const UUID& migrationUuid,
                                       RandomAccessOplogBuffer* oplogBuffer,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       Timestamp resumeBatchingTs,
                                       OpTime beginApplyingAfterOpTime)
    : AbstractAsyncComponent(executor.get(),
                             std::string("TenantOplogBatcher_") + migrationUuid.toString()),
      _oplogBuffer(oplogBuffer),
      _executor(executor),
      _resumeBatchingTs(resumeBatchingTs),
      _beginApplyingAfterOpTime(beginApplyingAfterOpTime) {}

TenantOplogBatcher::~TenantOplogBatcher() {
    shutdown();
    join();
}

void TenantOplogBatcher::_pushEntry(OperationContext* opCtx,
                                    TenantOplogBatch* batch,
                                    OplogEntry&& op) {
    uassert(4885606,
            str::stream() << "Prepared transactions are not supported for tenant migration."
                          << redact(op.toBSONForLogging()),
            !op.isPreparedCommit() &&
                (op.getCommandType() != OplogEntry::CommandType::kApplyOps || !op.shouldPrepare()));
    if (op.isTerminalApplyOps()) {
        if (op.getOpTime() <= _beginApplyingAfterOpTime) {
            // Fetched for the sake of retryable commitTransaction, don't need to apply.
            return;
        }
        // All applyOps entries are expanded and the expansions put in the batch expansion array.
        // The original applyOps is kept in the batch ops array.
        // This applies to multi-document transactions.
        auto expansionsIndex = batch->expansions.size();
        auto& curExpansion = batch->expansions.emplace_back();
        auto lastOpInTransactionBson = op.getEntry().toBSON();
        repl::ApplyOps::extractOperationsTo(op, lastOpInTransactionBson, &curExpansion);
        auto oplogPrevTsOption = op.getPrevWriteOpTimeInTransaction();
        if (oplogPrevTsOption && !oplogPrevTsOption->isNull()) {
            // Since 'extractOperationsTo' adds the operations to 'curExpansion' in chronological
            // order, but we traverse the oplog chain in reverse chronological order, we need to
            // reverse each set of operations from each 'applyOps', including the one in 'op', then
            // reverse the whole thing to get all the operations in chronological order.
            // We skip all this reversing in the common case of only one oplog entry.
            std::reverse(curExpansion.begin(), curExpansion.end());
            while (oplogPrevTsOption && !oplogPrevTsOption->isNull()) {
                auto txnOp = OplogEntry(uassertStatusOK(
                    _oplogBuffer->findByTimestamp(opCtx, oplogPrevTsOption->getTimestamp())));
                auto prevExpandedOpsEnd = curExpansion.size();
                repl::ApplyOps::extractOperationsTo(txnOp, lastOpInTransactionBson, &curExpansion);
                // This reverses results in the operations from this applyOps, so they are stored in
                // reverse chronological order.  Since we are traversing the chain in reverse order,
                // the whole array will end up in reverse order when we are done.
                std::reverse(curExpansion.begin() + prevExpandedOpsEnd, curExpansion.end());
                oplogPrevTsOption = txnOp.getPrevWriteOpTimeInTransaction();
            }
            std::reverse(curExpansion.begin(), curExpansion.end());
        }
        batch->ops.emplace_back(TenantOplogEntry(std::move(op), expansionsIndex));
    } else if (op.getPreImageOpTime() || op.getPostImageOpTime()) {
        uassert(5351001,
                str::stream() << "expected donor oplog entry with opTime: "
                              << op.getOpTime().toString() << ": " << redact(op.toBSONForLogging())
                              << " to have only one of " << OplogEntryBase::kPreImageOpTimeFieldName
                              << " or " << OplogEntryBase::kPostImageOpTimeFieldName,
                !op.getPreImageOpTime() || !op.getPostImageOpTime());
        OpTime imageOpTime =
            op.getPreImageOpTime() ? *op.getPreImageOpTime() : *op.getPostImageOpTime();
        // Almost all the time, this oplog entry will be the previous one in the batch.  However,
        // it may fall on a batch boundary or in unusual cases there may be oplog entries in
        // between.  In these cases we add the image directly before the oplog entry it refers to.
        // This is consistent with what shard migration does.  The oplog applier will ignore
        // image entries which are not directly followed by the entry referring to them.
        if (batch->ops.empty() || batch->ops.back().entry.getOpTime() != imageOpTime) {
            LOGV2_DEBUG(5351004,
                        1,
                        "Tenant Oplog Batcher reordering pre- or post- image for oplog entry",
                        "opTime"_attr = op.getOpTime(),
                        "imageOpTime"_attr = imageOpTime);
            auto swImageOp = _oplogBuffer->findByTimestamp(opCtx, imageOpTime.getTimestamp());
            if (swImageOp.getStatus() == ErrorCodes::NoSuchKey) {
                // If we don't find the pre/post image in the buffer, it means that the pre/post
                // image has an optime less than the startFetchingDonorOpTime and was never
                // fetched. This implies that there was a newer transaction number started in
                // the same session on the donor when the retryable write pre-fetch stage tried
                // to fetch oplog entries with optime less than the startFetchingDonorOpTime. In
                // that case, we don't need to support retrying the current findAndModify.
                // Therefore, use a dummy pre/post image.
                LOGV2(5535301,
                      "Tenant Oplog Batcher cannot find pre- or post- image",
                      "imageOpTime"_attr = imageOpTime);
            } else {
                batch->ops.emplace_back(TenantOplogEntry(uassertStatusOK(swImageOp)));
            }
        }
        batch->ops.emplace_back(TenantOplogEntry(std::move(op)));
    } else {
        batch->ops.emplace_back(TenantOplogEntry(std::move(op)));
    }
}

void TenantOplogBatcher::_consume(OperationContext* opCtx) {
    // This is just to get the op off the buffer; it's been peeked at and queued for application
    // already.
    // If we failed to get an op off the buffer, this means that shutdown() was called between the
    // consumer's calls to peek() and consume(). shutdown() cleared the buffer so there is nothing
    // for us to consume here. Since our postcondition is already met, it is safe to return
    // successfully.
    BSONObj opToPopAndDiscard;
    invariant(_oplogBuffer->tryPop(opCtx, &opToPopAndDiscard) || _isShuttingDown() || !isActive());
}

bool TenantOplogBatcher::_mustProcessIndividually(const OplogEntry& entry) {
    // See the comment of OplogBatcher::_getBatchActionForEntry() for details. The conditions
    // here are similar to the kProcessIndividually case in that function.
    if (entry.isCommand()) {
        return (entry.getCommandType() != OplogEntry::CommandType::kApplyOps) ||
            entry.shouldPrepare() || entry.isSingleOplogEntryTransactionWithCommand() ||
            entry.isEndOfLargeTransaction();
    }

    const auto nss = entry.getNss();
    return nss.mustBeAppliedInOwnOplogBatch();
}

StatusWith<TenantOplogBatch> TenantOplogBatcher::_readNextBatch(BatchLimits limits) {
    auto opCtx = cc().makeOperationContext();
    TenantOplogBatch batch;
    while (batch.ops.empty()) {
        bool hasData = false;
        while (!hasData) {
            hasData = _oplogBuffer->waitForData(Seconds(1));
            stdx::lock_guard lk(_mutex);
            if (!_isActive_inlock() || _isShuttingDown_inlock()) {
                return {ErrorCodes::CallbackCanceled, "Tenant oplog batcher shut down"};
            }
        }
        LOGV2_DEBUG(4885602,
                    1,
                    "Tenant Oplog Batcher reading batch",
                    "component"_attr = _getComponentName());
        BSONObj op;
        // We are guaranteed this loop will return at least one operation, because waitForData
        // returned true above.
        std::size_t totalOps = 0;
        std::uint32_t totalBytes = 0;
        while (_oplogBuffer->peek(opCtx.get(), &op)) {
            auto entry = OplogEntry(op);
            if (_mustProcessIndividually(entry)) {
                if (batch.ops.empty()) {
                    _pushEntry(opCtx.get(), &batch, std::move(entry));
                    _consume(opCtx.get());
                }
                // We end the batch before the entry that must be processed individually.  Since
                // we've only "peeked" it, it will be the first thing on the buffer for the next
                // batch.
                break;
            }
            auto opCount = OplogBatcher::getOpCount(entry);
            auto opBytes = entry.getRawObjSizeBytes();
            if (totalOps > 0 &&
                (totalOps + opCount > limits.ops || totalBytes + opBytes > limits.bytes)) {
                // Batch is done; no more operations fit.
                break;
            }
            totalOps += opCount;
            totalBytes += opBytes;
            _pushEntry(opCtx.get(), &batch, std::move(entry));
            _consume(opCtx.get());
        }
    }
    LOGV2_DEBUG(4885603,
                1,
                "Tenant Oplog Batcher read batch",
                "component"_attr = _getComponentName(),
                "size"_attr = batch.ops.size());
    return batch;
}

SemiFuture<TenantOplogBatch> TenantOplogBatcher::_scheduleNextBatch(WithLock, BatchLimits limits) {
    if (!_isActive_inlock() || _isShuttingDown_inlock()) {
        return SemiFuture<TenantOplogBatch>::makeReady(
            Status(ErrorCodes::CallbackCanceled, "Tenant oplog batcher has been shut down."));
    }
    auto pf = makePromiseFuture<TenantOplogBatch>();
    auto taskCompletionPromise = std::make_shared<Promise<TenantOplogBatch>>(std::move(pf.promise));
    _batchRequested = true;
    auto statusWithCbh =
        _executor->scheduleWork([this, limits, taskCompletionPromise, self = shared_from_this()](
                                    const executor::TaskExecutor::CallbackArgs& args) {
            if (!args.status.isOK()) {
                stdx::lock_guard lk(_mutex);
                _batchRequested = false;
                taskCompletionPromise->setError(args.status);
                if (_isShuttingDown_inlock()) {
                    _transitionToComplete_inlock();
                }
                return;
            }

            // Using makeReadyFutureWith here allows capturing exceptions.
            auto result = makeReadyFutureWith(
                [this, &limits, self = shared_from_this()] { return _readNextBatch(limits); });

            stdx::lock_guard lk(_mutex);
            // Fulfilling 'taskCompletionPromise' and resetting '_batchRequested' have to be done in
            // a single critical section to avoid failure due to "Cannot ask for already-requested
            // oplog fetcher batch".
            _batchRequested = false;
            taskCompletionPromise->setFrom(std::move(result));
            if (_isShuttingDown_inlock()) {
                _transitionToComplete_inlock();
            }
        });

    // If the batch fails to schedule, ensure we get a valid error code instead of a broken promise.
    if (!statusWithCbh.isOK()) {
        stdx::lock_guard lk(_mutex);
        _batchRequested = false;
        taskCompletionPromise->setError(statusWithCbh.getStatus());
        if (_isShuttingDown_inlock()) {
            _transitionToComplete_inlock();
        }
    }
    return std::move(pf.future).semi();
}

SemiFuture<TenantOplogBatch> TenantOplogBatcher::getNextBatch(BatchLimits limits) {
    stdx::lock_guard lk(_mutex);
    uassert(4885600, "Cannot ask for already-requested oplog fetcher batch.", !_batchRequested);
    uassert(4885601, "Batch limit in bytes must be greater than 0", limits.bytes > 0);
    uassert(4885607, "Batch limit in number of ops must be greater than 0", limits.ops > 0);
    return _scheduleNextBatch(lk, limits);
}

void TenantOplogBatcher::_doStartup_inlock() {
    LOGV2_DEBUG(
        4885604, 1, "Tenant Oplog Batcher starting up", "component"_attr = _getComponentName());
    if (!_resumeBatchingTs.isNull()) {
        auto opCtx = cc().makeOperationContext();
        uassert(5272303,
                str::stream() << "Error resuming oplog batcher",
                _oplogBuffer
                    ->seekToTimestamp(opCtx.get(),
                                      _resumeBatchingTs,
                                      RandomAccessOplogBuffer::SeekStrategy::kInexact)
                    .isOK());
        // Doing a 'seekToTimestamp' will not set the '_lastPoppedKey' on its own if a document
        // with '_resumeBatchingTs' exists in the buffer collection. We do a 'tryPop' here to set
        // '_lastPoppedKey' to equal '_resumeBatchingTs'.
        if (_oplogBuffer->findByTimestamp(opCtx.get(), _resumeBatchingTs).isOK()) {
            BSONObj opToPopAndDiscard;
            _oplogBuffer->tryPop(opCtx.get(), &opToPopAndDiscard);
        }
        LOGV2_DEBUG(5272306,
                    1,
                    "Tenant Oplog Batcher will resume batching from after timestamp",
                    "timestamp"_attr = _resumeBatchingTs);
    }
}

void TenantOplogBatcher::_doShutdown_inlock() noexcept {
    LOGV2_DEBUG(
        4885605, 1, "Tenant Oplog Batcher shutting down", "component"_attr = _getComponentName());
    if (!_batchRequested) {
        _transitionToComplete_inlock();
    }
    // If _batchRequested was true, we handle the _transitionToComplete when it becomes false.
}

}  // namespace repl
}  // namespace mongo
