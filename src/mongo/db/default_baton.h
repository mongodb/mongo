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

#pragma once

#include "mongo/db/baton.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/functional.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

#include <vector>

namespace mongo {

class OperationContext;

/**
 * The most basic Baton implementation.
 */
class DefaultBaton : public Baton {
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

    using Job = unique_function<void(stdx::unique_lock<stdx::mutex>)>;

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
    void _safeExecute(stdx::unique_lock<stdx::mutex> lk, Job job);

    void _notify(stdx::unique_lock<stdx::mutex>) noexcept;

    stdx::mutex _mutex;
    stdx::condition_variable _cv;
    bool _notified = false;
    bool _sleeping = false;

    OperationContext* _opCtx;

    AtomicWord<size_t> _nextTimerId;
    // Sorted in order of nearest -> furthest in future.
    std::multimap<Date_t, Timer> _timers;
    stdx::unordered_map<size_t, std::multimap<Date_t, Timer>::iterator> _timersById;

    std::vector<Task> _scheduled;
};

}  // namespace mongo
