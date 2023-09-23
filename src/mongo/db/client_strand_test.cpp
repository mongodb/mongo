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

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/executor_test_util.h"

namespace mongo {
namespace {

class ClientStrandTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
public:
    constexpr static auto kClientName1 = "foo";
    constexpr static auto kClientName2 = "bar";

    /**
     * Clean up any leftover thread_local pieces.
     */
    void releaseClient() {
        releaseThreadNameRef();
        if (haveClient()) {
            Client::releaseCurrent();
        }
    }

    void setUp() override {
        releaseClient();
    }

    void tearDown() override {
        releaseClient();
    }

    void assertStrandNotBound(const ClientStrandPtr& strand) {
        ASSERT_FALSE(haveClient());
        ASSERT_FALSE(strand->isBound());
    }

    void assertStrandBound(const ClientStrandPtr& strand) {
        // We have a Client.
        ASSERT_TRUE(haveClient());
        ASSERT_TRUE(strand->isBound());

        // The current Client and Thread have the correct name.
        auto client = strand->getClientPointer();
        ASSERT_EQ(client, Client::getCurrent());
        ASSERT_EQ(client->desc(), getThreadName());
    }
};

TEST_F(ClientStrandTest, CreateOnly) {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));

    // We have no bound Client.
    assertStrandNotBound(strand);

    // The Client should exist.
    ASSERT_TRUE(strand->getClientPointer());

    // The Client should reference its ClientStrand.
    ASSERT_EQ(ClientStrand::get(strand->getClientPointer()), strand);
}

TEST_F(ClientStrandTest, BindOnce) {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));

    // We have no bound Client.
    assertStrandNotBound(strand);

    {
        // Bind a single client
        auto guard = strand->bind();
        assertStrandBound(strand);

        // The guard allows us to get the Client.
        ASSERT_EQ(guard.get(), strand->getClientPointer());
    }

    // We have no bound Client again.
    assertStrandNotBound(strand);
}

TEST_F(ClientStrandTest, BindMultipleTimes) {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));

    // We have no bound Client.
    assertStrandNotBound(strand);

    for (auto i = 0; i < 10; ++i) {
        // Bind a bunch of times.

        {
            auto guard = strand->bind();
            assertStrandBound(strand);
        }

        // We have no bound Client again.
        assertStrandNotBound(strand);
        ASSERT_EQ(strand->getClientPointer()->desc(), getThreadName())
            << "We should retain the previous strand's name";
    }
}

TEST_F(ClientStrandTest, BindMultipleTimesAndDismiss) {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));

    // We have no bound Client.
    assertStrandNotBound(strand);

    auto guard = strand->bind();
    for (auto i = 0; i < 10; ++i) {
        assertStrandBound(strand);

        // Dismiss the current guard.
        guard.dismiss();
        assertStrandNotBound(strand);
        ASSERT_EQ(strand->getClientPointer()->desc(), getThreadName())
            << "We should retain the previous strand's name";

        // Assign a new guard.
        guard = strand->bind();
    }

    // At the end we have a strand bound.
    assertStrandBound(strand);
}

TEST_F(ClientStrandTest, BindLocalBeforeWorkerThread) {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));
    auto barrier = std::make_shared<unittest::Barrier>(2);

    // Set our state to an initial value. It is unsynchronized, but ClientStrand does synchronize,
    // thus it should pass TSAN.
    enum State {
        kStarted,
        kLocalThread,
        kWorkerThread,
    };
    State state = kStarted;

    assertStrandNotBound(strand);

    auto thread = stdx::thread([&, barrier] {
        // Wait for local thread to bind the strand.
        barrier->countDownAndWait();

        auto guard = strand->bind();
        assertStrandBound(strand);

        // We've acquired the strand after the local thread.
        ASSERT_EQ(state, kLocalThread);
        state = kWorkerThread;
    });

    {
        auto guard = strand->bind();
        assertStrandBound(strand);

        // Wait for the worker thread.
        barrier->countDownAndWait();

        // We've acquired the strand first.
        ASSERT_EQ(state, kStarted);
        state = kLocalThread;
    }

    thread.join();

    assertStrandNotBound(strand);

    // Bind one last time to synchronize the state.
    auto guard = strand->bind();

    // The worker thread acquired the strand last.
    ASSERT_EQ(state, kWorkerThread);
}

TEST_F(ClientStrandTest, BindLocalAfterWorkerThread) {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));
    auto barrier = std::make_shared<unittest::Barrier>(2);

    // Set our state to an initial value. It is unsynchronized, but ClientStrand does synchronize,
    // thus it should pass TSAN.
    enum State {
        kStarted,
        kLocalThread,
        kWorkerThread,
    };
    State state = kStarted;

    assertStrandNotBound(strand);

    auto thread = stdx::thread([&, barrier] {
        auto guard = strand->bind();
        assertStrandBound(strand);

        // Wait for local thread.
        barrier->countDownAndWait();

        // We've acquired the strand after the local thread.
        ASSERT_EQ(state, kStarted);
        state = kWorkerThread;
    });

    {
        // Wait for the worker thread to bind the strand.
        barrier->countDownAndWait();

        auto guard = strand->bind();
        assertStrandBound(strand);

        // We've acquired the strand first.
        ASSERT_EQ(state, kWorkerThread);
        state = kLocalThread;
    }

    thread.join();

    assertStrandNotBound(strand);

    // Bind one last time to synchronize the state.
    auto guard = strand->bind();
    assertStrandBound(strand);

    // The local thread acquired the strand last.
    ASSERT_EQ(state, kLocalThread);
}

TEST_F(ClientStrandTest, BindManyWorkerThreads) {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));

    constexpr size_t kCount = 10;
    auto barrier = std::make_shared<unittest::Barrier>(kCount);

    size_t threadsBound = 0;

    assertStrandNotBound(strand);

    std::vector<stdx::thread> threads;
    for (size_t i = 0; i < kCount; ++i) {
        threads.emplace_back([&, barrier] {
            // Wait for the herd.
            barrier->countDownAndWait();

            auto guard = strand->bind();
            assertStrandBound(strand);

            // This is technically atomic on x86 but TSAN should complain if it isn't synchronized.
            ++threadsBound;
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    assertStrandNotBound(strand);

    // Bind one last time to access the count.
    auto guard = strand->bind();
    assertStrandBound(strand);

    // We've been bound to the amount of threads we expected.
    ASSERT_EQ(threadsBound, kCount);
}

TEST_F(ClientStrandTest, SwapStrands) {
    auto strand1 = ClientStrand::make(getServiceContext()->makeClient(kClientName1));
    auto strand2 = ClientStrand::make(getServiceContext()->makeClient(kClientName2));

    assertStrandNotBound(strand1);
    assertStrandNotBound(strand2);

    for (size_t i = 0; i < 10; ++i) {
        // Alternate between binding strand1 and strand2. Start on strand2 so it has a different
        // thread name than the previous test.
        auto& strand = (i % 2 == 0) ? strand2 : strand1;

        ASSERT_NE(strand->getClientPointer()->desc(), getThreadName())
            << "We should be binding over the previous strand's name";
        auto guard = strand->bind();

        assertStrandBound(strand);
    }

    assertStrandNotBound(strand1);
    assertStrandNotBound(strand2);
}

TEST_F(ClientStrandTest, Executor) {
    constexpr size_t kCount = 10;

    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));

    assertStrandNotBound(strand);

    auto exec = strand->makeExecutor(InlineQueuedCountingExecutor::make());

    // Schedule a series of tasks onto the wrapped executor. Note that while this is running on the
    // local thread, this is not true recursive execution which would deadlock.
    size_t i = 0;
    unique_function<void(void)> reschedule;
    reschedule = [&] {
        exec->schedule([&](Status status) {
            invariant(status);
            assertStrandBound(strand);

            if (++i >= kCount) {
                // We've rescheduled enough.
                return;
            }

            reschedule();
        });
    };

    reschedule();
    assertStrandNotBound(strand);

    // Confirm we scheduled as many times as we expected.
    ASSERT_EQ(i, kCount);
}

DEATH_TEST_F(ClientStrandTest, ReplaceCurrentAfterBind, ClientStrand::kUnableToRecoverClient) {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));

    assertStrandNotBound(strand);

    auto guard = strand->bind();
    assertStrandBound(strand);

    // We need to capture the UniqueClient to avoid ABA pointer comparison issues with tcmalloc. In
    // practice, this failure mode is most likely if someone is using an AlternativeClientRegion,
    // which has its own issues.
    auto stolenClient = Client::releaseCurrent();
    Client::setCurrent(getServiceContext()->makeClient(kClientName2));

    // Dismiss the guard for an explicit failure point.
    guard.dismiss();
}

DEATH_TEST_F(ClientStrandTest, ReleaseCurrentAfterBind, "No client to release") {
    auto strand = ClientStrand::make(getServiceContext()->makeClient(kClientName1));

    assertStrandNotBound(strand);

    auto guard = strand->bind();
    assertStrandBound(strand);

    Client::releaseCurrent();

    // Dismiss the guard for an explicit failure point.
    guard.dismiss();
}

DEATH_TEST_F(ClientStrandTest, BindAfterBind, "Already have client on this thread") {
    auto strand1 = ClientStrand::make(getServiceContext()->makeClient(kClientName1));
    auto strand2 = ClientStrand::make(getServiceContext()->makeClient(kClientName2));

    assertStrandNotBound(strand1);
    assertStrandNotBound(strand2);

    // Bind our first strand.
    auto guard1 = strand1->bind();
    assertStrandBound(strand1);

    // Bind our second strand...and fail hard.
    auto guard2 = strand2->bind();
}

}  // namespace
}  // namespace mongo
