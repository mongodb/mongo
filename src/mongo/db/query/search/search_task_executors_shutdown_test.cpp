/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <mutex>

namespace mongo {
namespace executor {
namespace {

// Verifies that server shutdown does not hang indefinitely when something still holds a reference
// to the mongot task executor - which is exactly what an in-progress or idle $search cursor does.
// The original implementation waited unbounded for the reference to drop, blocking shutdown for
// tens of minutes until Atlas Automation SIGKILLed the process.
class SearchExecutorShutdownTest : public unittest::Test {
public:
    void setUp() override {
        _savedHost = globalMongotParams.host;
        _savedTimeoutMS = globalMongotParams.shutdownTimeoutMS.load();

        // A non-empty host makes shutdownSearchExecutorsIfNeeded() exercise the mongot executor
        // shutdown/destroy path. The address is never connected to in this test.
        globalMongotParams.host = "localhost:28000";
        _serviceCtx = ServiceContext::make();
    }

    void tearDown() override {
        _serviceCtx.reset();
        globalMongotParams.host = _savedHost;
        globalMongotParams.shutdownTimeoutMS.store(_savedTimeoutMS);
    }

protected:
    ServiceContext::UniqueServiceContext _serviceCtx;

private:
    std::string _savedHost;
    int _savedTimeoutMS;
};

TEST_F(SearchExecutorShutdownTest, ShutdownProceedsWhenAReferenceOutlivesTheTimeout) {
    // Simulate an idle $search cursor that still holds a strong reference to the mongot task
    // executor across shutdown. This reference would never be released by the time shutdown reaches
    // this point, so an unbounded wait hangs forever.
    auto outstandingRef = uassertStatusOK(getMongotTaskExecutor(_serviceCtx.get()));

    // Keep the bounded wait short so the test is fast.
    globalMongotParams.shutdownTimeoutMS.store(500);

    std::mutex mutex;
    stdx::condition_variable cv;
    bool shutdownReturned = false;

    stdx::thread shutdownThread([&] {
        shutdownSearchExecutorsIfNeeded(_serviceCtx.get());
        std::lock_guard<std::mutex> lk(mutex);
        shutdownReturned = true;
        cv.notify_all();
    });

    bool returnedInTime = false;
    {
        std::unique_lock<std::mutex> lk(mutex);
        // Generous bound relative to the 500ms timeout; the fix returns in ~0.5s, the bug never
        // returns.
        returnedInTime =
            cv.wait_for(lk, Seconds(30).toSystemDuration(), [&] { return shutdownReturned; });
    }

    // Always release the reference and join the thread before asserting, so the test never leaks a
    // running thread (even when the assertion below fails on buggy, unbounded-wait code).
    outstandingRef.reset();
    shutdownThread.join();

    ASSERT_TRUE(returnedInTime)
        << "shutdownSearchExecutorsIfNeeded() blocked past its timeout while a reference to the "
           "search task executor was still held";
}

TEST_F(SearchExecutorShutdownTest, ShutdownCompletesPromptlyWithNoOutstandingReferences) {
    globalMongotParams.shutdownTimeoutMS.store(500);

    std::mutex mutex;
    stdx::condition_variable cv;
    bool shutdownReturned = false;

    stdx::thread shutdownThread([&] {
        shutdownSearchExecutorsIfNeeded(_serviceCtx.get());
        std::lock_guard<std::mutex> lk(mutex);
        shutdownReturned = true;
        cv.notify_all();
    });

    bool returnedInTime = false;
    {
        std::unique_lock<std::mutex> lk(mutex);
        returnedInTime =
            cv.wait_for(lk, Seconds(30).toSystemDuration(), [&] { return shutdownReturned; });
    }
    shutdownThread.join();

    // With no outstanding references the wait loop should exit immediately, well before the
    // timeout.
    ASSERT_TRUE(returnedInTime);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
