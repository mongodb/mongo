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


#include "mongo/db/repl/oplog_applier.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/processinfo.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

NoopOplogApplierObserver noopOplogApplierObserver;

OplogApplier::OplogApplier(executor::TaskExecutor* executor,
                           OplogBuffer* oplogBuffer,
                           Observer* observer,
                           const Options& options)
    : _executor(executor), _oplogBuffer(oplogBuffer), _observer(observer), _options(options) {
    _oplogBatcher = std::make_unique<OplogApplierBatcher>(this, oplogBuffer);
}

OplogBuffer* OplogApplier::getBuffer() const {
    return _oplogBuffer;
}

Future<void> OplogApplier::startup() {
    auto pf = makePromiseFuture<void>();
    auto callback = [this, promise = std::move(pf.promise)](
                        const executor::TaskExecutor::CallbackArgs& args) mutable noexcept {
        invariant(args.status);
        LOGV2(21224, "Starting oplog application");
        _run(_oplogBuffer);
        LOGV2(21225, "Finished oplog application");
        promise.setWith([] {});
    };
    invariant(_executor->scheduleWork(std::move(callback)).getStatus());
    return std::move(pf.future);
}

void OplogApplier::shutdown() {
    // Shutdown will hang if this failpoint is enabled.
    if (globalFailPointRegistry().find("rsSyncApplyStop")->shouldFail()) {
        LOGV2_FATAL_NOTRACE(40304, "Turn off rsSyncApplyStop before attempting clean shutdown");
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _inShutdown = true;
}

bool OplogApplier::inShutdown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _inShutdown;
}

void OplogApplier::waitForSpace(OperationContext* opCtx, const OplogBuffer::Cost& cost) {
    _oplogBuffer->waitForSpace(opCtx, cost);
}

/**
 * Pushes operations read from sync source into oplog buffer.
 */
void OplogApplier::enqueue(OperationContext* opCtx,
                           std::vector<OplogEntry>::const_iterator begin,
                           std::vector<OplogEntry>::const_iterator end,
                           boost::optional<const OplogBuffer::Cost&> cost) {
    OplogBuffer::Batch batch;
    for (auto i = begin; i != end; ++i) {
        batch.push_back(i->getEntry().getRaw());
    }
    enqueue(opCtx, batch.cbegin(), batch.cend(), cost);
}

void OplogApplier::enqueue(OperationContext* opCtx,
                           OplogBuffer::Batch::const_iterator begin,
                           OplogBuffer::Batch::const_iterator end,
                           boost::optional<const OplogBuffer::Cost&> cost) {
    static Occasionally sampler;
    if (sampler.tick()) {
        LOGV2_DEBUG(21226,
                    2,
                    "Oplog apply buffer size",
                    "oplogApplyBufferSizeBytes"_attr = _oplogBuffer->getSize());
    }
    _oplogBuffer->push(opCtx, begin, end, cost);
}

StatusWith<OpTime> OplogApplier::applyOplogBatch(OperationContext* opCtx,
                                                 std::vector<OplogEntry> ops) {
    _observer->onBatchBegin(ops);
    auto lastApplied = _applyOplogBatch(opCtx, std::move(ops));
    _observer->onBatchEnd(lastApplied, {});
    return lastApplied;
}

StatusWith<OplogApplierBatch> OplogApplier::getNextApplierBatch(OperationContext* opCtx,
                                                                const BatchLimits& batchLimits,
                                                                Milliseconds waitToFillBatch) {
    return _oplogBatcher->getNextApplierBatch(opCtx, batchLimits, waitToFillBatch);
}

const OplogApplier::Options& OplogApplier::getOptions() const {
    return _options;
}

const OpTime& OplogApplier::getMinValid() {
    return _minValid;
}

void OplogApplier::setMinValid(const OpTime& minValid) {
    _minValid = minValid;
}

std::unique_ptr<ThreadPool> makeReplWorkerPool() {
    // Reduce content pinned in cache by single oplog batch on small machines by reducing the number
    // of threads of ReplWriter to reduce the number of concurrent open WT transactions.
    if (replWriterThreadCount < replWriterMinThreadCount) {
        LOGV2_FATAL_NOTRACE(
            5605400,
            "replWriterMinThreadCount must be less than or equal to replWriterThreadCount",
            "replWriterMinThreadCount"_attr = replWriterMinThreadCount,
            "replWriterThreadCount"_attr = replWriterThreadCount);
    }
    auto numberOfThreads =
        std::min(replWriterThreadCount, 2 * static_cast<int>(ProcessInfo::getNumAvailableCores()));
    return makeReplWorkerPool(numberOfThreads);
}

std::unique_ptr<ThreadPool> makeReplWorkerPool(int threadCount) {
    return makeReplWorkerPool(threadCount, "ReplWriterWorker"_sd);
}

std::unique_ptr<ThreadPool> makeReplWorkerPool(int threadCount,
                                               StringData name,
                                               bool isKillableByStepdown) {
    ThreadPool::Options options;
    options.threadNamePrefix = name + "-";
    options.poolName = name + "ThreadPool";
    options.minThreads =
        replWriterMinThreadCount < threadCount ? replWriterMinThreadCount : threadCount;
    options.maxThreads = static_cast<size_t>(threadCount);
    options.onCreateThread = [isKillableByStepdown](const std::string&) {
        Client::initThread(getThreadName(),
                           getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                           Client::noSession(),
                           ClientOperationKillableByStepdown{isKillableByStepdown});
        auto client = Client::getCurrent();
        AuthorizationSession::get(*client)->grantInternalAuthorization();
    };
    auto pool = std::make_unique<ThreadPool>(options);
    pool->startup();
    return pool;
}

}  // namespace repl
}  // namespace mongo
