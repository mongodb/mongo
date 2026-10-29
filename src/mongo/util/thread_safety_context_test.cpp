// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/util/thread_safety_context.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

class ThreadSafetyContextTest : public unittest::Test {
public:
    void setUp() override {
        // Always start tests with a clean context
        resetContext();
    }

    void tearDown() override {
        // Clear the context for the next test
        resetContext();
    }

private:
    void resetContext() {
        auto context = ThreadSafetyContext::getThreadSafetyContext();
        context->_isSingleThreaded.store(true);
        context->_safeToCreateThreads.store(true);
    }
};

TEST_F(ThreadSafetyContextTest, CreateThreadsWithNoSafetyContext) {
    constexpr auto threadCount = 16;
    std::vector<stdx::thread> threads;

    for (auto i = 0; i < threadCount; i++) {
        threads.emplace_back([] { sleepFor(Milliseconds(10)); });
    }

    for (auto i = 0; i < threadCount; i++) {
        threads[i].join();
    }
}

using ThreadSafetyContextTestDeathTest = ThreadSafetyContextTest;
DEATH_TEST_F(ThreadSafetyContextTestDeathTest,
             CreateThreadsAfterForbidingMultiThreading,
             "invariant") {
    ThreadSafetyContext::getThreadSafetyContext()->forbidMultiThreading();
    // Must terminate after starting the thread
    auto thread = stdx::thread([] { sleepFor(Milliseconds(50)); });
    thread.join();
}

DEATH_TEST_F(ThreadSafetyContextTestDeathTest,
             ForbidMultiThreadingAfterCreatingThreads,
             "invariant") {
    auto thread = stdx::thread([]() {});

    // Wait for the thread to return before proceeding with the test
    thread.join();

    ThreadSafetyContext::getThreadSafetyContext()->forbidMultiThreading();
    // Must never reach here or the test fails
}

TEST_F(ThreadSafetyContextTest, CreateThreadsAfterSafetyContext) {
    ThreadSafetyContext::getThreadSafetyContext()->forbidMultiThreading();
    // Do something inside the single-threaded context
    sleepFor(Milliseconds(10));
    ThreadSafetyContext::getThreadSafetyContext()->allowMultiThreading();

    constexpr auto threadCount = 16;
    std::vector<stdx::thread> threads;

    for (auto i = 0; i < threadCount; i++) {
        threads.emplace_back([] { sleepFor(Milliseconds(10)); });
    }

    for (auto i = 0; i < threadCount; i++) {
        threads[i].join();
    }
}

TEST_F(ThreadSafetyContextTest, SingleThreadedContext) {
    ASSERT(ThreadSafetyContext::getThreadSafetyContext()->isSingleThreaded());
    stdx::thread([]() {
        ASSERT(!ThreadSafetyContext::getThreadSafetyContext()->isSingleThreaded());
    }).join();
    ASSERT(!ThreadSafetyContext::getThreadSafetyContext()->isSingleThreaded());
}

}  // namespace mongo
