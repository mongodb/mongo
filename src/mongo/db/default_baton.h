// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/baton.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

#include <mutex>
#include <vector>

namespace mongo {

class OperationContext;

/**
 * The most basic Baton implementation.
 */
class [[MONGO_MOD_PUBLIC]] DefaultBaton : public Baton {
public:
    explicit DefaultBaton(OperationContext* opCtx);

    ~DefaultBaton() override;

    void schedule(Task func) override;

    Future<void> waitUntil(Date_t expiration, const CancellationToken& token) override;

    void notify() noexcept override;

    Waitable::TimeoutState run_until(ClockSource* clkSource, Date_t oldDeadline) noexcept override;

    void run(ClockSource* clkSource) noexcept override;

private:
    struct Timer {
        size_t id;
        Promise<void> promise;
    };
    void detachImpl() override;

    using Job = unique_function<void(std::unique_lock<std::mutex>)>;

    /**
     * Invokes a job with exclusive access to the baton's internals.
     *
     * If the baton is currently sleeping (i.e., `_sleeping` is `true`), the sleeping thread owns
     * the baton, so we schedule the job and notify the sleeping thread to wake up and run the job.
     *
     * Otherwise, take exclusive access and run the job on the current thread.
     *
     * Note that `_safeExecute()` will throw if the baton has been detached.
     *
     * Also note that the job may not run inline, and may get scheduled to run by the baton, so it
     * should never throw.
     */
    void _safeExecute(std::unique_lock<std::mutex> lk, Job job);

    void _notify(std::unique_lock<std::mutex>) noexcept;

    std::mutex _mutex;
    stdx::condition_variable _cv;
    bool _notified = false;
    bool _sleeping = false;

    OperationContext* _opCtx;

    Atomic<size_t> _nextTimerId;
    // Sorted in order of nearest -> furthest in future.
    std::multimap<Date_t, Timer> _timers;
    stdx::unordered_map<size_t, std::multimap<Date_t, Timer>::iterator> _timersById;

    std::vector<Task> _scheduled;
};

}  // namespace mongo
