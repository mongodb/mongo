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
#include <vector>

#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/thread_safety_context.h"
#include "mongo/util/time_support.h"

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

DEATH_TEST_F(ThreadSafetyContextTest, CreateThreadsAfterForbidingMultiThreading, "invariant") {
    ThreadSafetyContext::getThreadSafetyContext()->forbidMultiThreading();
    // Must terminate after starting the thread
    auto thread = stdx::thread([] { sleepFor(Milliseconds(50)); });
    thread.join();
}

DEATH_TEST_F(ThreadSafetyContextTest, ForbidMultiThreadingAfterCreatingThreads, "invariant") {
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

}  // namespace mongo
