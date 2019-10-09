/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_applier.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

NoopOplogApplierObserver noopOplogApplierObserver;

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

OplogApplier::OplogApplier(executor::TaskExecutor* executor,
                           OplogBuffer* oplogBuffer,
                           Observer* observer,
                           const Options& options)
    : _executor(executor), _oplogBuffer(oplogBuffer), _observer(observer), _options(options) {}

OplogBuffer* OplogApplier::getBuffer() const {
    return _oplogBuffer;
}

Future<void> OplogApplier::startup() {
    auto pf = makePromiseFuture<void>();
    auto callback =
        [ this, promise = std::move(pf.promise) ](const CallbackArgs& args) mutable noexcept {
        invariant(args.status);
        log() << "Starting oplog application";
        _run(_oplogBuffer);
        log() << "Finished oplog application";
        promise.setWith([] {});
    };
    invariant(_executor->scheduleWork(std::move(callback)).getStatus());
    return std::move(pf.future);
}

void OplogApplier::shutdown() {
    // Shutdown will hang if this failpoint is enabled.
    if (globalFailPointRegistry().find("rsSyncApplyStop")->shouldFail()) {
        severe() << "Turn off rsSyncApplyStop before attempting clean shutdown";
        fassertFailedNoTrace(40304);
    }

    stdx::lock_guard<Latch> lock(_mutex);
    _inShutdown = true;
}

bool OplogApplier::inShutdown() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _inShutdown;
}

void OplogApplier::waitForSpace(OperationContext* opCtx, std::size_t size) {
    _oplogBuffer->waitForSpace(opCtx, size);
}

/**
 * Pushes operations read from sync source into oplog buffer.
 */
void OplogApplier::enqueue(OperationContext* opCtx,
                           Operations::const_iterator begin,
                           Operations::const_iterator end) {
    OplogBuffer::Batch batch;
    for (auto i = begin; i != end; ++i) {
        batch.push_back(i->getRaw());
    }
    enqueue(opCtx, batch.cbegin(), batch.cend());
}

void OplogApplier::enqueue(OperationContext* opCtx,
                           OplogBuffer::Batch::const_iterator begin,
                           OplogBuffer::Batch::const_iterator end) {
    static Occasionally sampler;
    if (sampler.tick()) {
        LOG(2) << "oplog buffer has " << _oplogBuffer->getSize() << " bytes";
    }
    _oplogBuffer->push(opCtx, begin, end);
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
 * Returns whether an oplog entry represents an applyOps which doesn't imply prepare.
 * It could be a partial transaction oplog entry, an implicit commit applyOps or an applyOps outside
 * of transaction.
 */
bool isUnpreparedApplyOps(const OplogEntry& entry) {
    return entry.getCommandType() == OplogEntry::CommandType::kApplyOps && !entry.shouldPrepare();
}

/**
 * Returns true if this oplog entry must be processed in its own batch and cannot be grouped with
 * other entries.
 *
 * Commands must be processed one at a time. The exceptions to this are unprepared applyOps, because
 * applyOps oplog entries are effectively containers for CRUD operations, and unprepared
 * commitTransaction, because that also expands to CRUD operations. Therefore, it is safe to batch
 * applyOps commands with CRUD operations when reading from the oplog buffer.
 *
 * Oplog entries on 'system.views' should also be processed one at a time. View catalog immediately
 * reflects changes for each oplog entry so we can see inconsistent view catalog if multiple oplog
 * entries on 'system.views' are being applied out of the original order.
 *
 * Process updates to 'admin.system.version' individually as well so the secondary's FCV when
 * processing each operation matches the primary's when committing that operation.
 */
bool mustProcessStandalone(const OplogEntry& entry) {
    if (entry.isCommand()) {
        if (isUnpreparedCommit(entry)) {
            return false;
        } else if (isUnpreparedApplyOps(entry)) {
            return false;
        }
        return true;
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

StatusWith<OplogApplier::Operations> OplogApplier::getNextApplierBatch(
    OperationContext* opCtx, const BatchLimits& batchLimits) {
    if (batchLimits.ops == 0) {
        return Status(ErrorCodes::InvalidOptions, "Batch size must be greater than 0.");
    }

    std::size_t totalOps = 0;
    std::uint32_t totalBytes = 0;
    Operations ops;
    BSONObj op;
    while (_oplogBuffer->peek(opCtx, &op)) {
        auto entry = OplogEntry(op);

        // Check for oplog version change.
        if (entry.getVersion() != OplogEntry::kOplogVersion) {
            std::string message = str::stream()
                << "expected oplog version " << OplogEntry::kOplogVersion << " but found version "
                << entry.getVersion() << " in oplog entry: " << redact(entry.toBSON());
            severe() << message;
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

        if (mustProcessStandalone(entry)) {
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

StatusWith<OpTime> OplogApplier::multiApply(OperationContext* opCtx, Operations ops) {
    _observer->onBatchBegin(ops);
    auto lastApplied = _multiApply(opCtx, std::move(ops));
    _observer->onBatchEnd(lastApplied, {});
    return lastApplied;
}

const OplogApplier::Options& OplogApplier::getOptions() const {
    return _options;
}

void OplogApplier::_consume(OperationContext* opCtx, OplogBuffer* oplogBuffer) {
    // This is just to get the op off the queue; it's been peeked at and queued for application
    // already.
    // If we failed to get an op off the queue, this means that shutdown() was called between the
    // consumer's calls to peek() and consume(). shutdown() cleared the buffer so there is nothing
    // for us to consume here. Since our postcondition is already met, it is safe to return
    // successfully.
    BSONObj opToPopAndDiscard;
    invariant(oplogBuffer->tryPop(opCtx, &opToPopAndDiscard) || inShutdown());
}

std::unique_ptr<ThreadPool> makeReplWriterPool() {
    return makeReplWriterPool(replWriterThreadCount);
}

std::unique_ptr<ThreadPool> makeReplWriterPool(int threadCount) {
    ThreadPool::Options options;
    options.threadNamePrefix = "repl-writer-worker-";
    options.poolName = "repl writer worker Pool";
    options.maxThreads = options.minThreads = static_cast<size_t>(threadCount);
    options.onCreateThread = [](const std::string&) {
        Client::initThread(getThreadName());
        AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());
    };
    auto pool = std::make_unique<ThreadPool>(options);
    pool->startup();
    return pool;
}

std::size_t getBatchLimitOplogEntries() {
    return std::size_t(replBatchLimitOperations.load());
}

std::size_t getBatchLimitOplogBytes(OperationContext* opCtx, StorageInterface* storageInterface) {
    auto oplogMaxSizeResult =
        storageInterface->getOplogMaxSize(opCtx, NamespaceString::kRsOplogNamespace);
    auto oplogMaxSize = fassert(40301, oplogMaxSizeResult);
    return std::min(oplogMaxSize / 10, std::size_t(replBatchLimitBytes.load()));
}

}  // namespace repl
}  // namespace mongo
