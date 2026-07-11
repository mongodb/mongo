// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/oplog_applier.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/repl_worker_pool_thread_count.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/observable_mutex_registry.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {
using namespace std::literals::string_view_literals;

NoopOplogApplierObserver noopOplogApplierObserver;

OplogApplier::OplogApplier(executor::TaskExecutor* executor,
                           OplogBuffer* oplogBuffer,
                           Observer* observer,
                           const Options& options)
    : _executor(executor), _oplogBuffer(oplogBuffer), _observer(observer), _options(options) {
    ObservableMutexRegistry::get().add("oplogApplierMutex", _mutex);
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

    std::lock_guard lock(_mutex);
    _inShutdown = true;
}

bool OplogApplier::inShutdown() const {
    std::lock_guard lock(_mutex);
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


namespace {

std::unique_ptr<ThreadPool> makeReplWorkerPool(size_t threadCount,
                                               std::string_view name,
                                               bool isKillableByStepdown) {
    auto pool = ThreadPool::make({
        .poolName = fmt::format("{}ThreadPool", name),
        .threadNamePrefix = fmt::format("{}-", name),
        .minThreads = std::min(getMinThreadCountForReplWorkerPool(), threadCount),
        .maxThreads = threadCount,
        .onCreateThread =
            [isKillableByStepdown](const std::string&) {
                Client::initThread(getThreadName(),
                                   getGlobalServiceContext()->getService(),
                                   Client::noSession(),
                                   ClientOperationKillableByStepdown{isKillableByStepdown});
                auto client = Client::getCurrent();
                AuthorizationSession::get(*client)->grantInternalAuthorization();
            },
    });
    pool->startup();
    return pool;
}

}  // namespace

std::unique_ptr<ThreadPool> makeReplWorkerPool() {
    return makeReplWorkerPool(getThreadCountForReplWorkerPool());
}

std::unique_ptr<ThreadPool> makeReplWorkerPool(size_t threadCount) {
    return makeReplWorkerPool(threadCount, "ReplWriterWorker"sv, false);
}

}  // namespace repl
}  // namespace mongo
