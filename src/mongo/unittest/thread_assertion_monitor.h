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

#pragma once

#include <exception>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo::unittest {

/**
 * Worker threads cannot use ASSERT exceptions, because they'll normally crash the
 * process. We provide a way for a worker to transfer ASSERT failures to the
 * main thread to be rethrown from there.
 *
 * The main thread has to be structured such that it's waiting
 * on this monitor while all other work happens in workers.
 */
class ThreadAssertionMonitor {

public:
    // Monitor will `wait()` on destruction, blocking until a `notifyDone()` call has occurred.
    ~ThreadAssertionMonitor() noexcept(false) {
        wait();
    }

    /** Spawn and return a `stdx::thread` that invokes `f` as if by `exec(f)`. */
    template <typename F>
    stdx::thread spawn(F&& f) {
        return stdx::thread{[this, f = std::move(f)]() mutable { exec(std::move(f)); }};
    }

    /** Spawn a thread that will invoke monitor.notifyDone()` when it finishes. */
    template <typename F>
    stdx::thread spawnController(F&& f) {
        return spawn([this, f = std::move(f)]() mutable {
            auto notifyDoneGuard = makeGuard([this] { notifyDone(); });
            exec(std::move(f));
        });
    }

    /** Invokes `f` inside a try/catch that routes any ASSERT failures to the monitor. */
    template <typename F>
    void exec(F&& f) {
        try {
            std::invoke(std::forward<F>(f));
        } catch (const unittest::TestAssertionFailureException&) {
            // Transport ASSERT failures to the monitor.
            bool notify = false;
            {
                stdx::unique_lock lk(_mu);  // NOLINT
                if (!_ex) {
                    _ex = std::current_exception();
                    notify = true;
                }
            }
            if (notify)
                _cv.notify_one();
        }
    }

    void notifyDone() {
        {
            stdx::unique_lock lk(_mu);
            _done = true;
        }
        _cv.notify_one();
    }

    // Blocks until `notifyDone` is called.
    // Throws if an ASSERT exception was reported by any exec invocation.
    void wait() {
        stdx::unique_lock lk(_mu);
        do {
            _cv.wait(lk, [&] { return _done || _ex; });
            if (_ex)
                std::rethrow_exception(std::exchange(_ex, nullptr));
        } while (!_done);
    }

private:
    stdx::mutex _mu;  // NOLINT
    stdx::condition_variable _cv;
    std::exception_ptr _ex;
    bool _done = false;
};

/**
 * Covers probably most cases of multithreaded tests.
 * The body of a test can be passed in as an `f` that
 * accepts a `unittest::ThreadAssertionMonitor&`.
 * `f(monitor)` will to become the body of a "controller" thread.
 *
 * Any `stdx::thread(...)` constructors in the test must then be
 * converted to `monitor.spawn(...)` calls. Then the ASSERT
 * calls in the test will be managed by the monitor and propagate
 * to be rethrown from the main thread, cleanly failing the unit test.
 *
 * The controller thread is joined inside this function,
 * but user code is still responsible for joining the spawned
 * worker threads as usual.
 */
template <typename F>
void threadAssertionMonitoredTest(F&& f) {
    ThreadAssertionMonitor monitor;
    monitor.spawnController([&, f] { std::invoke(f, monitor); }).join();
}

}  // namespace mongo::unittest
