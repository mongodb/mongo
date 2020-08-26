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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/platform/basic.h"

#include "mongo/db/repl/tenant_oplog_batcher.h"

#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/oplog_batcher.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace repl {
TenantOplogBatcher::TenantOplogBatcher(const std::string& tenantId,
                                       RandomAccessOplogBuffer* oplogBuffer,
                                       std::shared_ptr<executor::TaskExecutor> executor)
    : AbstractAsyncComponent(executor.get(), std::string("TenantOplogBatcher_") + tenantId),
      _oplogBuffer(oplogBuffer),
      _executor(executor) {}

TenantOplogBatcher::~TenantOplogBatcher() {
    shutdown();
    join();
}

void TenantOplogBatcher::_pushEntry(OperationContext* opCtx,
                                    TenantOplogBatch* batch,
                                    OplogEntry&& op) {
    uassert(4885606,
            str::stream() << "Prepared transactions are not supported for tenant migration."
                          << redact(op.toBSON()),
            !op.isPreparedCommit() &&
                (op.getCommandType() != OplogEntry::CommandType::kApplyOps || !op.shouldPrepare()));
    if (op.isTerminalApplyOps()) {
        // All applyOps entries are expanded and the expansions put in the batch expansion array.
        // The original applyOps is kept in the batch ops array.
        // This applies to both multi-document transactions and atomic applyOps.
        auto expansionsIndex = batch->expansions.size();
        auto& curExpansion = batch->expansions.emplace_back();
        auto lastOpInTransactionBson = op.toBSON();
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
            if (OplogBatcher::mustProcessIndividually(entry)) {
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
    _promise = std::move(pf.promise);
    _batchRequested = true;
    auto statusWithCbh =
        _executor->scheduleWork([this, limits](const executor::TaskExecutor::CallbackArgs& args) {
            if (!args.status.isOK()) {
                stdx::lock_guard lk(_mutex);
                _promise->setError(args.status);
                return;
            }
            // Using makeReadyFutureWith here allows capturing exceptions.
            auto result = makeReadyFutureWith([this, &limits] { return _readNextBatch(limits); });
            stdx::lock_guard lk(_mutex);
            _batchRequested = false;
            _promise->setFrom(std::move(result));
            if (_isShuttingDown_inlock()) {
                _transitionToComplete_inlock();
            }
        });

    // If the batch fails to schedule, ensure we get a valid error code instead of a broken promise.
    if (!statusWithCbh.isOK()) {
        _promise->setError(statusWithCbh.getStatus());
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

Status TenantOplogBatcher::_doStartup_inlock() noexcept {
    LOGV2_DEBUG(
        4885604, 1, "Tenant Oplog Batcher starting up", "component"_attr = _getComponentName());
    return Status::OK();
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
