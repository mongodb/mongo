// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/mock_reactor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <vector>

namespace mongo::transport {
namespace {

class MockReactorTest : public unittest::Test {
protected:
    MockReactor& reactor() {
        return *_reactor;
    }

    Date_t now() {
        return _reactor->now();
    }

private:
    std::shared_ptr<MockReactor> _reactor = std::make_shared<MockReactor>();
};

TEST_F(MockReactorTest, TimerFiresOnAdvance) {
    auto start = now();
    ASSERT_EQ(Date_t(reactor().systemTime()), start);

    auto timer = reactor().makeTimer();
    auto fut = timer->waitUntil(start + Milliseconds(100));
    ASSERT_FALSE(fut.isReady());
    ASSERT_EQ(reactor().pendingTimerCount(), 1);

    reactor().advanceTime(Milliseconds(99));
    ASSERT_EQ(now(), start + Milliseconds(99));
    ASSERT_FALSE(fut.isReady());
    ASSERT_EQ(reactor().pendingTimerCount(), 1);

    reactor().advanceTime(Milliseconds(1));
    ASSERT_EQ(now(), start + Milliseconds(100));
    ASSERT_EQ(reactor().clock().now(), now());
    ASSERT_TRUE(fut.isReady());
    ASSERT_OK(fut.getNoThrow());
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

// The next two tests pin ClockSourceMock behaviour rather than MockReactor logic: alarms fire
// earliest-first within a single advance, and every ClockSourceMock instance drives the same
// process-global clock. MockReactor's contract (and NetworkInterfaceTL's unit tests) rely on both,
// and clock_source_mock_test.cpp does not assert either, so they are kept here.
TEST_F(MockReactorTest, TimersFireInDeadlineOrder) {
    std::vector<int> order;
    auto t1 = reactor().makeTimer();
    auto t2 = reactor().makeTimer();
    auto t3 = reactor().makeTimer();

    // Registered out of deadline order on purpose.
    auto f2 = t2->waitUntil(now() + Milliseconds(20)).then([&] { order.push_back(2); });
    auto f3 = t3->waitUntil(now() + Milliseconds(30)).then([&] { order.push_back(3); });
    auto f1 = t1->waitUntil(now() + Milliseconds(10)).then([&] { order.push_back(1); });
    ASSERT_EQ(reactor().pendingTimerCount(), 3);

    // One advance that covers all three deadlines still fires them earliest-first.
    reactor().advanceTime(Milliseconds(30));
    ASSERT_OK(f1.getNoThrow());
    ASSERT_OK(f2.getNoThrow());
    ASSERT_OK(f3.getNoThrow());
    ASSERT_EQ(order, (std::vector<int>{1, 2, 3}));
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

TEST_F(MockReactorTest, TimerFiresWhenSharedClockAdvancedElsewhere) {
    // Timers are alarms on the process-global mock clock, so advancing it through any other
    // ClockSourceMock instance (e.g. the one installed on a ServiceContext) fires them too.
    auto timer = reactor().makeTimer();
    auto fut = timer->waitUntil(now() + Milliseconds(100));
    ASSERT_FALSE(fut.isReady());

    ClockSourceMock otherClock;
    otherClock.advance(Milliseconds(100));
    ASSERT_TRUE(fut.isReady());
    ASSERT_OK(fut.getNoThrow());
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

// A wait is settled exactly once, by whichever of cancel() or the clock alarm gets there first;
// the loser must be a no-op rather than a double-settle of the promise.
TEST_F(MockReactorTest, CancelSettlesTheWaitAtMostOnce) {
    auto timer = reactor().makeTimer();

    // Nothing armed: cancel() has nothing to settle.
    timer->cancel();
    timer->cancel();
    ASSERT_EQ(reactor().pendingTimerCount(), 0);

    // Cancel first, then the alarm fires: the alarm must not resurrect the wait.
    auto fut = timer->waitUntil(now() + Milliseconds(100));
    ASSERT_FALSE(fut.isReady());
    timer->cancel();
    ASSERT_TRUE(fut.isReady());
    ASSERT_EQ(fut.getNoThrow(), ErrorCodes::CallbackCanceled);
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
    reactor().advanceTime(Milliseconds(100));
    ASSERT_EQ(reactor().pendingTimerCount(), 0);

    // Alarm first, then cancel(): the already-fulfilled promise must be left alone.
    auto fired = timer->waitUntil(now() + Milliseconds(10));
    reactor().advanceTime(Milliseconds(10));
    ASSERT_OK(fired.getNoThrow());
    timer->cancel();
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

TEST_F(MockReactorTest, RearmCancelsPrevious) {
    auto timer = reactor().makeTimer();
    auto first = timer->waitUntil(now() + Milliseconds(100));
    auto second = timer->waitUntil(now() + Milliseconds(10));

    ASSERT_TRUE(first.isReady());
    ASSERT_EQ(first.getNoThrow(), ErrorCodes::CallbackCanceled);
    ASSERT_FALSE(second.isReady());
    ASSERT_EQ(reactor().pendingTimerCount(), 1);

    reactor().advanceTime(Milliseconds(10));
    ASSERT_OK(second.getNoThrow());
}

TEST_F(MockReactorTest, DestructorCancels) {
    auto timer = reactor().makeTimer();
    auto fut = timer->waitUntil(now() + Milliseconds(100));
    ASSERT_FALSE(fut.isReady());

    timer.reset();
    ASSERT_TRUE(fut.isReady());
    ASSERT_EQ(fut.getNoThrow(), ErrorCodes::CallbackCanceled);
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

TEST_F(MockReactorTest, WaitUntilAfterStopReturnsShutdownInProgress) {
    auto timer = reactor().makeTimer();
    reactor().stop();
    ASSERT_TRUE(reactor().isStopped());

    auto fut = timer->waitUntil(now() + Milliseconds(100));
    ASSERT_TRUE(fut.isReady());
    ASSERT_EQ(fut.getNoThrow(), ErrorCodes::ShutdownInProgress);
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

TEST_F(MockReactorTest, TimerOutlivingReactorIsSettledAndSafe) {
    ClockSourceMock clock;
    auto reactor = std::make_shared<MockReactor>();
    auto timer = reactor->makeTimer();
    auto fut = timer->waitUntil(reactor->now() + Milliseconds(100));
    ASSERT_FALSE(fut.isReady());

    reactor.reset();
    ASSERT_TRUE(fut.isReady());
    ASSERT_EQ(fut.getNoThrow(), ErrorCodes::CallbackCanceled);

    // The alarm is still queued in the clock, but its wait is settled so firing it is a no-op.
    clock.advance(Milliseconds(100));

    // The timer must remain usable (if useless) after its reactor is gone.
    timer->cancel();
    auto late = timer->waitUntil(clock.now() + Milliseconds(1));
    ASSERT_EQ(late.getNoThrow(), ErrorCodes::ShutdownInProgress);
}

TEST_F(MockReactorTest, ScheduleRunsInlineOnReactorThread) {
    ASSERT_FALSE(reactor().onReactorThread());

    bool outerRan = false;
    bool innerRan = false;
    reactor().schedule([&](Status s) {
        ASSERT_OK(s);
        ASSERT_TRUE(reactor().onReactorThread());
        // Nested schedule from within a task must not trip the ThreadIdGuard invariant.
        reactor().schedule([&](Status s2) {
            ASSERT_OK(s2);
            ASSERT_TRUE(reactor().onReactorThread());
            innerRan = true;
        });
        ASSERT_TRUE(innerRan);
        ASSERT_TRUE(reactor().onReactorThread());
        outerRan = true;
    });

    ASSERT_TRUE(outerRan);
    ASSERT_FALSE(reactor().onReactorThread());
}

TEST_F(MockReactorTest, ScheduleAfterStopStillRunsInline) {
    reactor().stop();
    bool ran = false;
    reactor().schedule([&](Status s) {
        ASSERT_OK(s);
        ran = true;
    });
    ASSERT_TRUE(ran);
}

TEST_F(MockReactorTest, TimerContinuationRunsOnReactorThread) {
    auto timer = reactor().makeTimer();
    bool sawReactorThread = false;
    auto fut = timer->waitUntil(now() + Milliseconds(5)).then([&] {
        sawReactorThread = reactor().onReactorThread();
    });
    reactor().advanceTime(Milliseconds(5));
    ASSERT_OK(fut.getNoThrow());
    ASSERT_TRUE(sawReactorThread);
    ASSERT_FALSE(reactor().onReactorThread());
}

TEST_F(MockReactorTest, RunBlocksUntilStop) {
    Notification<void> runReturned;
    stdx::thread ioThread([&] {
        reactor().run();
        reactor().drain();
        runReturned.set();
    });

    // What matters for NetworkInterfaceTL::shutdown() is that stop() makes run() return so the
    // io thread can be joined. Whether run() blocked beforehand cannot be asserted without a
    // timing wait, so it is not.
    reactor().stop();
    runReturned.get();
    ioThread.join();
    ASSERT_TRUE(reactor().isStopped());
}

TEST_F(MockReactorTest, ContinuationMayCancelAnotherTimerDuringAdvance) {
    auto t1 = reactor().makeTimer();
    auto t2 = reactor().makeTimer();

    auto f2 = t2->waitUntil(now() + Milliseconds(20));
    // t1 fires first and cancels t2 before t2's own deadline.
    auto f1 = t1->waitUntil(now() + Milliseconds(10)).then([&] { t2->cancel(); });

    reactor().advanceTime(Milliseconds(20));
    ASSERT_OK(f1.getNoThrow());
    ASSERT_EQ(f2.getNoThrow(), ErrorCodes::CallbackCanceled);
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

TEST_F(MockReactorTest, ContinuationMayArmDueTimerDuringAdvance) {
    auto t1 = reactor().makeTimer();
    auto t2 = reactor().makeTimer();

    // t1 fires at +10 and arms t2 for +15, which is already due because the clock reads +20 by the
    // time t1's continuation runs, so ClockSourceMock fires it inside setAlarm(). The deadline is
    // computed from `start` rather than now() so that it is already due.
    auto start = now();
    Future<void> f2;
    auto f1 = t1->waitUntil(start + Milliseconds(10)).then([&] {
        ASSERT_EQ(now(), start + Milliseconds(20));
        f2 = t2->waitUntil(start + Milliseconds(15));
    });

    reactor().advanceTime(Milliseconds(20));
    ASSERT_OK(f1.getNoThrow());
    ASSERT_TRUE(f2.valid());
    ASSERT_TRUE(f2.isReady());
    ASSERT_OK(f2.getNoThrow());
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

TEST_F(MockReactorTest, ContinuationMayRearmSameTimerDuringAdvance) {
    auto timer = reactor().makeTimer();
    int fires = 0;
    Future<void> second;
    auto first = timer->waitUntil(now() + Milliseconds(10)).then([&] {
        ++fires;
        second = timer->waitUntil(now() + Milliseconds(10)).then([&] { ++fires; });
    });

    reactor().advanceTime(Milliseconds(10));
    ASSERT_OK(first.getNoThrow());
    ASSERT_EQ(fires, 1);
    ASSERT_FALSE(second.isReady());
    ASSERT_EQ(reactor().pendingTimerCount(), 1);

    reactor().advanceTime(Milliseconds(10));
    ASSERT_OK(second.getNoThrow());
    ASSERT_EQ(fires, 2);
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
}

TEST_F(MockReactorTest, SleepForResolvesOnAdvance) {
    // Reactor::sleepFor is implemented over makeTimer() + schedule(), so this exercises both
    // through the ExecutorFuture machinery that AsyncTry uses.
    CancellationSource source;
    auto fut = reactor().sleepFor(Milliseconds(50), source.token());
    ASSERT_FALSE(fut.isReady());

    reactor().advanceTime(Milliseconds(49));
    ASSERT_FALSE(fut.isReady());
    reactor().advanceTime(Milliseconds(1));
    ASSERT_OK(std::move(fut).getNoThrow());
}

TEST_F(MockReactorTest, SleepForHonoursCancellation) {
    CancellationSource source;
    auto fut = reactor().sleepFor(Milliseconds(50), source.token());
    ASSERT_FALSE(fut.isReady());

    source.cancel();
    ASSERT_EQ(std::move(fut).getNoThrow(), ErrorCodes::CallbackCanceled);
}

}  // namespace
}  // namespace mongo::transport
