// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mongo::sbe {

/**
 * A single worker thread, owned by an ExchangeWorkerPool, which runs one job at a time on behalf of
 * whoever currently leases it. The thread has its own Client, created when the thread is leased,
 * and it stays parked on a condition variable in between jobs.
 */
class ExchangeWorker {
public:
    ExchangeWorker();

    // Stops and joins the thread. The caller must make sure no job is outstanding (the pool does
    // so by calling waitIdle() before it takes a worker back).
    ~ExchangeWorker();

    ExchangeWorker(const ExchangeWorker&) = delete;
    ExchangeWorker& operator=(const ExchangeWorker&) = delete;

    /**
     * Hands 'newJob' to this worker's thread and returns immediately. At most one job may be
     * outstanding at a time; callers are responsible for not calling submit() again before the
     * previous job has signaled its own completion (e.g. via the Promise/Future it captured).
     */
    void submit(unique_function<void()> newJob);

    /**
     * Like submit(), but runs 'func' on this worker's thread through Promise::setWith() and returns
     * a Future that is fulfilled with its result, or with the exception it threw.
     */
    template <typename Func>
    auto submitAsPromise(Func&& func) -> Future<FutureContinuationResult<Func>> {
        auto [promise, future] = makePromiseFuture<FutureContinuationResult<Func>>();
        submit([func = std::forward<Func>(func), promise = std::move(promise)]() mutable {
            promise.setWith(std::move(func));
        });
        return std::move(future);
    }

    /**
     * Blocks until this worker has no job outstanding, i.e. until whatever was last submit()'d (if
     * anything) has finished running.
     */
    void waitIdle();

    ServiceContext::UniqueOperationContext opCtxHolder;

private:
    void threadLoop();

    std::mutex _mutex;
    stdx::condition_variable _cv;
    unique_function<void()> _job;
    bool _hasJob = false;
    bool _busy = false;
    bool _stopRequested = false;

    // Declared last so that every other member is fully initialized by the time the thread which
    // reads them starts running.
    stdx::thread _thread;
};

/**
 * Process-wide pool of ExchangeWorker threads, from which parallel query plans lease the threads
 * they run their producers on. Workers handed back to the pool are parked rather than joined, so a
 * query only pays for thread creation when it finds the pool empty.
 */
class ExchangeWorkerPool {
public:
    static ExchangeWorkerPool& get(ServiceContext* svcCtx);

    /**
     * Returns a leased worker back to the pool (or destroys it if the pool is already holding
     * enough idle workers).
     */
    struct Returner {
        ExchangeWorkerPool* pool = nullptr;

        void operator()(ExchangeWorker* worker) const;
    };

    // A lease on a worker: it goes back to the pool when the handle is destroyed or reset.
    using Handle = std::unique_ptr<ExchangeWorker, Returner>;

    ExchangeWorkerPool() = default;
    ~ExchangeWorkerPool();

    /**
     * Leases up to 'count' workers, reusing parked ones if there are any and creating new threads
     * otherwise.
     */
    std::vector<Handle> acquire(OperationContext* mainOpCtx, size_t count);

    /**
     * Joins every parked worker and stops parking the ones released afterwards. Called when the
     * owning ServiceContext is torn down, while it is still legal for the workers' Clients to
     * exist. No worker may be leased out at that point.
     */
    void shutdown();

    // Total number of worker threads ever created by this pool. Only meant for testing and
    // diagnostics.
    size_t createdCount() const;

    // Number of workers currently parked in the pool, i.e. not leased to anybody. Only meant for
    // testing and diagnostics.
    size_t idleCount() const;

    // Number of workers currently leased out to a query. Only meant for testing and diagnostics.
    size_t leasedCount() const;

    // Appends the 'created', 'idle' and 'inUse' worker counts to 'bob', as reported in
    // serverStatus (and therefore FTDC) under the 'exchangeWorkerPool' section.
    void appendStats(BSONObjBuilder& bob) const;

private:
    void release(ExchangeWorker* worker);

    mutable std::mutex _mutex;
    std::vector<std::unique_ptr<ExchangeWorker>> _idle;
    size_t _created = 0;
    size_t _leased = 0;
    bool _shutdown = false;
};

}  // namespace mongo::sbe
