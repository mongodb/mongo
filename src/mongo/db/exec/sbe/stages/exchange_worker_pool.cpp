// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/exchange_worker_pool.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/util/assert_util.h"

#include <functional>
#include <utility>

#include <fmt/format.h>

namespace mongo::sbe {
namespace {
const auto getExchangeWorkerPool = ServiceContext::declareDecoration<ExchangeWorkerPool>();

ServiceContext::ConstructorActionRegisterer exchangeWorkerPoolShutdown{
    "ExchangeWorkerPoolShutdown",
    [](ServiceContext*) {},
    [](ServiceContext* svcCtx) {
        ExchangeWorkerPool::get(svcCtx).shutdown();
    }};

// Reports the state of the pool in serverStatus, and therefore in FTDC.
class ExchangeWorkerPoolServerStatusSection final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder bob;
        ExchangeWorkerPool::get(opCtx->getServiceContext()).appendStats(bob);
        return bob.obj();
    }
};

auto& exchangeWorkerPoolServerStatusSection =
    *ServerStatusSectionBuilder<ExchangeWorkerPoolServerStatusSection>("exchangeWorkerPool")
         .forShard();
}  // namespace

ExchangeWorker::ExchangeWorker() {
    _thread = stdx::thread([this] { threadLoop(); });
}

ExchangeWorker::~ExchangeWorker() {
    {
        std::lock_guard lk(_mutex);
        tassert(
            13436100, "destroying an ExchangeWorker with a job outstanding", !_hasJob && !_busy);
        _stopRequested = true;
        _cv.notify_one();
    }
    _thread.join();
}

void ExchangeWorker::submit(unique_function<void()> newJob) {
    std::lock_guard lk(_mutex);
    _job = std::move(newJob);
    _hasJob = true;
    _cv.notify_one();
}

void ExchangeWorker::waitIdle() {
    std::unique_lock lk(_mutex);
    _cv.wait(lk, [&] { return !_hasJob && !_busy; });
}

void ExchangeWorker::threadLoop() {
    while (true) {
        unique_function<void()> currentJob;
        {
            std::unique_lock lk(_mutex);
            _cv.wait(lk, [&] { return _hasJob || _stopRequested; });
            if (!_hasJob && _stopRequested) {
                return;
            }
            currentJob = std::move(_job);
            _hasJob = false;
            _busy = true;
        }
        currentJob();
        currentJob = nullptr;
        {
            std::lock_guard lk(_mutex);
            _busy = false;
        }
        _cv.notify_all();
    }
}

ExchangeWorkerPool& ExchangeWorkerPool::get(ServiceContext* svcCtx) {
    return getExchangeWorkerPool(svcCtx);
}

ExchangeWorkerPool::~ExchangeWorkerPool() {
    // Every lease is held by a query plan, which cannot outlive the ServiceContext this pool is
    // attached to.
    tassert(
        13436101, "destroying an ExchangeWorkerPool with workers still leased out", _leased == 0);
}

void ExchangeWorkerPool::shutdown() {
    std::vector<std::unique_ptr<ExchangeWorker>> toJoin;
    {
        std::lock_guard lk(_mutex);
        tassert(13436102,
                "shutting down an ExchangeWorkerPool with workers still leased out",
                _leased == 0);
        _shutdown = true;
        toJoin.swap(_idle);
    }
    // Joins every parked thread, outside of the lock.
    toJoin.clear();
}

std::vector<ExchangeWorkerPool::Handle> ExchangeWorkerPool::acquire(OperationContext* mainOpCtx,
                                                                    size_t count) {
    std::vector<Handle> workers;
    {
        std::lock_guard lk(_mutex);
        tassert(13436104, "ExchangeWorkerPool has been shut down", !_shutdown);
        // The knob can be lowered at runtime below the number of workers already leased out.
        const size_t maxWorkers = internalQuerySBEExchangeMaxWorkers.loadRelaxed();
        // The maximum number of threads that can be acquired now is the headroom between _leased
        // and maxWorkers.
        size_t maxAvailableThreads =
            (_leased > maxWorkers) ? 0 : std::min(maxWorkers, maxWorkers - _leased);
        // Discard idle threads in excess of the maximum allowed.
        if (_idle.size() > maxAvailableThreads) {
            _created -= _idle.size() - maxAvailableThreads;
            _idle.resize(maxAvailableThreads);
        }
        size_t i;
        for (i = 0; !_idle.empty() && i < count; i++) {
            workers.emplace_back(Handle{std::move(_idle.back()).release(), Returner{this}});
            _idle.pop_back();
        }
        for (; i < std::min(count, maxAvailableThreads); i++) {
            workers.emplace_back(Handle{new ExchangeWorker(), Returner{this}});
            _created++;
        }
        _leased += workers.size();
    }

    // Initialize the Client and give it a name connected to the name of the current thread.
    std::vector<Future<Client*>> futures;
    for (size_t i = 0; i < workers.size(); i++) {
        auto name = fmt::format("{}-{}", getThreadName(), i + 1);
        futures.emplace_back(workers[i]->submitAsPromise([name, service = mainOpCtx->getService()] {
            Client::initThread(name, service);
            return Client::getCurrent();
        }));
    }
    // Create the OperationContext and store it in the worker object.
    for (size_t i = 0; i < workers.size(); i++) {
        ForwardableOperationMetadata fom(mainOpCtx);
        workers[i]->opCtxHolder = futures[i].get()->makeOperationContext();
        fom.setOn(workers[i]->opCtxHolder.get());
    }

    return workers;
}

void ExchangeWorkerPool::Returner::operator()(ExchangeWorker* worker) const {
    pool->release(worker);
}

void ExchangeWorkerPool::release(ExchangeWorker* worker) {
    std::unique_ptr<ExchangeWorker> owned(worker);

    // Whatever the last lease submitted must be done before anybody else can drive this thread.
    owned->waitIdle();

    // Release the OperationContext, so that it's removed from the Client we are going to free in
    // the worker thread.
    owned->opCtxHolder.reset();

    // Schedule a new job to clear the current Client from the thread.
    owned->submit([]() { Client::releaseCurrent(); });
    owned->waitIdle();

    std::lock_guard lk(_mutex);
    tassert(13436103, "releasing an unaccounted worker", _leased > 0);
    --_leased;
    // If we have created more threads than the maximum allowed, destroy this thread
    const size_t maxWorkers = internalQuerySBEExchangeMaxWorkers.loadRelaxed();
    if (_created > maxWorkers) {
        _created--;
    } else {
        _idle.push_back(std::move(owned));
    }
    // Otherwise 'owned' goes out of scope and the thread is joined.
}

size_t ExchangeWorkerPool::createdCount() const {
    std::lock_guard lk(_mutex);
    return _created;
}

size_t ExchangeWorkerPool::idleCount() const {
    std::lock_guard lk(_mutex);
    return _idle.size();
}

size_t ExchangeWorkerPool::leasedCount() const {
    std::lock_guard lk(_mutex);
    return _leased;
}

void ExchangeWorkerPool::appendStats(BSONObjBuilder& bob) const {
    std::lock_guard lk(_mutex);
    bob.append("created", static_cast<long long>(_created));
    bob.append("idle", static_cast<long long>(_idle.size()));
    bob.append("inUse", static_cast<long long>(_leased));
}

}  // namespace mongo::sbe
