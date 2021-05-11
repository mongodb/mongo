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

#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

/**
 * This decoration increments a set of global counters on creation and destruction.
 */
class TestDecoration {
public:
    TestDecoration() {
        ASSERT(!ThreadContext::get())
            << "ThreadContext decorations should be created before the ThreadContext is set";

        _created.fetchAndAdd(1);
    };

    ~TestDecoration() {
        _destroyed.fetchAndAdd(1);

        if (ThreadContext::get()) {
            // We should only be able to reference a ThreadContext in our destructor if our
            // lifetime was extended to be off thread.
            _destroyedOffThread.fetchAndAdd(1);
        }
    }

    static auto created() {
        return _created.load();
    }

    static auto destroyed() {
        return _destroyed.load();
    }

    static auto destroyedOffThread() {
        return _destroyedOffThread.load();
    }

private:
    static inline AtomicWord<size_t> _created{0};
    static inline AtomicWord<size_t> _destroyed{0};
    static inline AtomicWord<size_t> _destroyedOffThread{0};
};

const auto getThreadTestDecoration = ThreadContext::declareDecoration<TestDecoration>();

class ThreadContextTest : public unittest::Test {
public:
    void setUp() override;
    void tearDown() override;

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
        auto t = stdx::thread(std::forward<F>(f));
        t.join();
    }

    /**
     * Get the amount of TestDecoration instances created since the start of this test.
     */
    auto decorationsCreated() const {
        return TestDecoration::created() - _created;
    }

    /**
     * Get the amount of TestDecoration instances destroyed since the start of this test.
     */
    auto decorationsDestroyed() const {
        return TestDecoration::destroyed() - _destroyed;
    }

    /**
     * Get the amount of TestDecoration instances destroyed off thread since the start of this test.
     */
    auto decorationsDestroyedOffThread() const {
        return TestDecoration::destroyedOffThread() - _destroyedOffThread;
    }

private:
    size_t _created;
    size_t _destroyed;
    size_t _destroyedOffThread;
};

void ThreadContextTest::setUp() {
    _created = TestDecoration::created();
    _destroyed = TestDecoration::destroyed();
    _destroyedOffThread = TestDecoration::destroyedOffThread();
}

void ThreadContextTest::tearDown() {
    ASSERT_EQ(decorationsCreated(), decorationsDestroyed())
        << "Each created decoration should also be destroyed";
    ASSERT_GTE(decorationsDestroyed(), decorationsDestroyedOffThread())
        << "We can never have more decorations destroyed off thread than we have made in total";
}

TEST_F(ThreadContextTest, HasLocalThreadContext) {
    auto context = getThreadContext();

    // Since this is the local thread, there should be no difference since the start of the test.
    ASSERT_EQ(decorationsCreated(), 0);
    ASSERT_EQ(decorationsDestroyed(), 0);
    ASSERT_EQ(decorationsDestroyedOffThread(), 0);
}

TEST_F(ThreadContextTest, HasNewThreadContext) {
    launchAndJoinThread([&] {
        auto context = getThreadContext();

        ASSERT_EQ(decorationsCreated(), 1);
        ASSERT_EQ(decorationsDestroyed(), 0);
        ASSERT_EQ(decorationsDestroyedOffThread(), 0);
    });

    ASSERT_EQ(decorationsCreated(), 1);
    ASSERT_EQ(decorationsDestroyed(), 1);
    ASSERT_EQ(decorationsDestroyedOffThread(), 0);
}

TEST_F(ThreadContextTest, CanExtendThreadContextLifetime) {
    boost::intrusive_ptr<ThreadContext> context;

    launchAndJoinThread([&] {
        context = getThreadContext();

        ASSERT_EQ(decorationsCreated(), 1);
        ASSERT_EQ(decorationsDestroyed(), 0);
        ASSERT_EQ(decorationsDestroyedOffThread(), 0);
    });

    assertNotAlive(context);

    ASSERT_EQ(decorationsCreated(), 1);
    ASSERT_EQ(decorationsDestroyed(), 0);
    ASSERT_EQ(decorationsDestroyedOffThread(), 0);

    context.reset();

    // The context is gone.
    ASSERT_EQ(decorationsCreated(), 1);
    ASSERT_EQ(decorationsDestroyed(), 1);
    ASSERT_EQ(decorationsDestroyedOffThread(), 1);
}

TEST_F(ThreadContextTest, AreThreadContextsUnique) {
    boost::intrusive_ptr<ThreadContext> contextA;
    boost::intrusive_ptr<ThreadContext> contextB;

    launchAndJoinThread([&] {
        contextA = getThreadContext();

        ASSERT_EQ(decorationsCreated(), 1);
        ASSERT_EQ(decorationsDestroyed(), 0);
        ASSERT_EQ(decorationsDestroyedOffThread(), 0);
    });

    launchAndJoinThread([&] {
        contextB = getThreadContext();

        ASSERT_EQ(decorationsCreated(), 2);
        ASSERT_EQ(decorationsDestroyed(), 0);
        ASSERT_EQ(decorationsDestroyedOffThread(), 0);
    });

    assertNotAlive(contextA);
    assertNotAlive(contextB);

    ASSERT_NE(ThreadContext::get()->threadId(), contextA->threadId())
        << "The context for the local thread should be different than the one for thread A";
    ASSERT_NE(contextA->threadId(), contextB->threadId())
        << "The context for thread A should be different than the one for thread B";

    contextA.reset();
    contextB.reset();

    ASSERT_EQ(decorationsCreated(), 2);
    ASSERT_EQ(decorationsDestroyed(), 2);
    ASSERT_EQ(decorationsDestroyedOffThread(), 2);
}

// This check runs in pre-init and then we check it in a test post-init.
const bool gHasAThreadContextPreInit = [] { return static_cast<bool>(ThreadContext::get()); }();
TEST_F(ThreadContextTest, HasNoPreInitThreadContext) {
    ASSERT(!gHasAThreadContextPreInit);
}

}  // namespace
}  // namespace mongo
