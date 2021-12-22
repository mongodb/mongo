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

#include "mongo/platform/basic.h"

#include "mongo/util/thread_context.h"

#include <boost/optional.hpp>
#include <fmt/format.h>

#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {
namespace {

using namespace fmt::literals;

struct Counters {
    Counters(uint64_t c, uint64_t d, uint64_t dot)
        : created(c), destroyed(d), destroyedOffThread(dot) {}

    friend bool operator==(const Counters& a, const Counters& b) {
        auto lens = [](auto& v) { return std::tie(v.created, v.destroyed, v.destroyedOffThread); };
        return lens(a) == lens(b);
    }
    friend bool operator!=(const Counters& a, const Counters& b) {
        return !(a == b);
    }

    friend std::ostream& operator<<(std::ostream& os, const Counters& v) {
        return os << "(created:{}, destroyed:{}, destroyedOffThread:{})"_format(
                   v.created, v.destroyed, v.destroyedOffThread);
    }

    uint64_t created;
    uint64_t destroyed;
    uint64_t destroyedOffThread;
};

synchronized_value gCounters{Counters{0, 0, 0}};

/**
 * This decoration increments a set of global counters on creation and destruction.
 */
class TestDecoration {
public:
    TestDecoration() {
        ASSERT(!ThreadContext::get())
            << "ThreadContext decorations should be created before the ThreadContext is set";

        ++gCounters->created;
    };

    ~TestDecoration() {
        ++gCounters->destroyed;

        if (ThreadContext::get()) {
            // We should only be able to reference a ThreadContext in our destructor if our
            // lifetime was extended to be off thread.
            ++gCounters->destroyedOffThread;
        }
    }
};

const auto getThreadTestDecoration = ThreadContext::declareDecoration<TestDecoration>();

class ThreadContextTest : public unittest::Test {
public:
    void setUp() override {
        ThreadContext::get();  // Ensure a ThreadContext for the main thread.
        _monitor.emplace();
        *gCounters = {0, 0, 0};
    }

    void tearDown() override {
        _monitor->notifyDone();
        _monitor.reset();
        auto endCount = gCounters.get();
        ASSERT_EQ(endCount.created, endCount.destroyed);
        ASSERT_GTE(endCount.destroyed, endCount.destroyedOffThread);
    }

    /**
     * Get the ThreadContext for the current thread and assert that it is valid and alive.
     */
    auto getThreadContext() {
        auto context = ThreadContext::get();

        ASSERT(context);
        ASSERT(context->isAlive());

        return context;
    }

    /**
     * Verify that the given ThreadContext is valid but not alive.
     */
    void assertNotAlive(boost::intrusive_ptr<ThreadContext> context) {
        ASSERT(context);
        ASSERT(!context->isAlive());
    }

    /**
     * Launch a thread and then immediately join it.
     */
    template <typename F>
    void launchAndJoinThread(F&& f) {
        _monitor->spawn(std::forward<F>(f)).join();
    }

    boost::optional<unittest::ThreadAssertionMonitor> _monitor;
};

TEST_F(ThreadContextTest, HasLocalThreadContext) {
    auto context = getThreadContext();

    // Since this is the local thread, there should be no difference since the start of the test.
    ASSERT_EQ(gCounters.get(), Counters(0, 0, 0));
}

TEST_F(ThreadContextTest, HasNewThreadContext) {
    launchAndJoinThread([&] {
        auto context = getThreadContext();
        ASSERT_EQ(gCounters.get(), Counters(1, 0, 0));
    });
    ASSERT_EQ(gCounters.get(), Counters(1, 1, 0));
}

TEST_F(ThreadContextTest, CanExtendThreadContextLifetime) {
    boost::intrusive_ptr<ThreadContext> context;

    launchAndJoinThread([&] {
        context = getThreadContext();
        ASSERT_EQ(gCounters.get(), Counters(1, 0, 0));
    });

    assertNotAlive(context);

    ASSERT_EQ(gCounters.get(), Counters(1, 0, 0));

    context.reset();

    // The context is gone.
    ASSERT_EQ(gCounters.get(), Counters(1, 1, 1));
}

TEST_F(ThreadContextTest, AreThreadContextsUnique) {
    boost::intrusive_ptr<ThreadContext> contextA;
    boost::intrusive_ptr<ThreadContext> contextB;

    launchAndJoinThread([&] {
        contextA = getThreadContext();
        ASSERT_EQ(gCounters.get(), Counters(1, 0, 0));
    });

    launchAndJoinThread([&] {
        contextB = getThreadContext();
        ASSERT_EQ(gCounters.get(), Counters(2, 0, 0));
    });

    assertNotAlive(contextA);
    assertNotAlive(contextB);

    ASSERT_NE(ThreadContext::get()->threadId(), contextA->threadId())
        << "The context for the local thread should be different than the one for thread A";
    ASSERT_NE(contextA->threadId(), contextB->threadId())
        << "The context for thread A should be different than the one for thread B";

    contextA.reset();
    contextB.reset();

    ASSERT_EQ(gCounters.get(), Counters(2, 2, 2));
}

// This check runs in pre-init and then we check it in a test post-init.
const bool gHasAThreadContextPreInit = !!ThreadContext::get();

TEST_F(ThreadContextTest, HasNoPreInitThreadContext) {
    ASSERT(!gHasAThreadContextPreInit);
}

}  // namespace
}  // namespace mongo
