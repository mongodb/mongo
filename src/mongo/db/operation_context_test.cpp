/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

std::ostream& operator<<(std::ostream& os, stdx::cv_status cvStatus) {
    switch (cvStatus) {
        case stdx::cv_status::timeout:
            return os << "timeout";
        case stdx::cv_status::no_timeout:
            return os << "no_timeout";
        default:
            MONGO_UNREACHABLE;
    }
}

std::ostream& operator<<(std::ostream& os, stdx::future_status futureStatus) {
    switch (futureStatus) {
        case stdx::future_status::ready:
            return os << "ready";
        case stdx::future_status::deferred:
            return os << "deferred";
        case stdx::future_status::timeout:
            return os << "timeout";
        default:
            MONGO_UNREACHABLE;
    }
}

class OperationDeadlineTests : public unittest::Test {
public:
    void setUp() {
        service = stdx::make_unique<ServiceContextNoop>();
        service->setFastClockSource(stdx::make_unique<SharedClockSourceAdapter>(mockClock));
        service->setPreciseClockSource(stdx::make_unique<SharedClockSourceAdapter>(mockClock));
        service->setTickSource(stdx::make_unique<TickSourceMock>());
        client = service->makeClient("OperationDeadlineTest");
    }

    const std::shared_ptr<ClockSourceMock> mockClock = std::make_shared<ClockSourceMock>();
    std::unique_ptr<ServiceContext> service;
    ServiceContext::UniqueClient client;
};

TEST_F(OperationDeadlineTests, OperationDeadlineExpiration) {
    auto txn = client->makeOperationContext();
    txn->setDeadlineAfterNowBy(Seconds{1});
    mockClock->advance(Milliseconds{500});
    ASSERT_OK(txn->checkForInterruptNoAssert());

    // 1ms before relative deadline reports no interrupt
    mockClock->advance(Milliseconds{499});
    ASSERT_OK(txn->checkForInterruptNoAssert());

    // Exactly at deadline reports no interrupt, because setDeadlineAfterNowBy adds one clock
    // precision unit to the deadline, to ensure that the deadline does not expire in less than the
    // requested amount of time.
    mockClock->advance(Milliseconds{1});
    ASSERT_OK(txn->checkForInterruptNoAssert());

    // Since the mock clock's precision is 1ms, at test start + 1001 ms, we expect
    // checkForInterruptNoAssert to return ExceededTimeLimit.
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, txn->checkForInterruptNoAssert());

    // Also at times greater than start + 1001ms, we expect checkForInterruptNoAssert to keep
    // returning ExceededTimeLimit.
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, txn->checkForInterruptNoAssert());
}

template <typename D>
void assertLargeRelativeDeadlineLikeInfinity(Client& client, D maxTime) {
    auto txn = client.makeOperationContext();
    txn->setDeadlineAfterNowBy(maxTime);
    ASSERT_FALSE(txn->hasDeadline()) << "Tried to set maxTime to " << maxTime;
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesHours) {
    ASSERT_FALSE(client->makeOperationContext()->hasDeadline());
    assertLargeRelativeDeadlineLikeInfinity(*client, Hours::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMinutes) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Minutes::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesSeconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Seconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMilliseconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Milliseconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMicroseconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Microseconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesNanoseconds) {
    // Nanoseconds::max() is less than Microseconds::max(), so it is possible to set
    // a deadline of that duration.
    auto txn = client->makeOperationContext();
    txn->setDeadlineAfterNowBy(Nanoseconds::max());
    ASSERT_TRUE(txn->hasDeadline());
    ASSERT_EQ(mockClock->now() + mockClock->getPrecision() +
                  duration_cast<Milliseconds>(Nanoseconds::max()),
              txn->getDeadline());
}

TEST_F(OperationDeadlineTests, WaitForMaxTimeExpiredCV) {
    auto txn = client->makeOperationContext();
    txn->setDeadlineByDate(mockClock->now());
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, txn->waitForConditionOrInterruptNoAssert(cv, lk));
}

TEST_F(OperationDeadlineTests, WaitForMaxTimeExpiredCVWithWaitUntilSet) {
    auto txn = client->makeOperationContext();
    txn->setDeadlineByDate(mockClock->now());
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit,
              txn->waitForConditionOrInterruptNoAssertUntil(cv, lk, mockClock->now() + Seconds{10})
                  .getStatus());
}

TEST_F(OperationDeadlineTests, WaitForKilledOpCV) {
    auto txn = client->makeOperationContext();
    txn->markKilled();
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_EQ(ErrorCodes::Interrupted, txn->waitForConditionOrInterruptNoAssert(cv, lk));
}

TEST_F(OperationDeadlineTests, WaitForUntilExpiredCV) {
    auto txn = client->makeOperationContext();
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT(stdx::cv_status::timeout ==
           unittest::assertGet(
               txn->waitForConditionOrInterruptNoAssertUntil(cv, lk, mockClock->now())));
}

TEST_F(OperationDeadlineTests, WaitForUntilExpiredCVWithMaxTimeSet) {
    auto txn = client->makeOperationContext();
    txn->setDeadlineByDate(mockClock->now() + Seconds{10});
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT(stdx::cv_status::timeout ==
           unittest::assertGet(
               txn->waitForConditionOrInterruptNoAssertUntil(cv, lk, mockClock->now())));
}

TEST_F(OperationDeadlineTests, DuringWaitMaxTimeExpirationDominatesUntilExpiration) {
    auto txn = client->makeOperationContext();
    txn->setDeadlineByDate(mockClock->now());
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT(ErrorCodes::ExceededTimeLimit ==
           txn->waitForConditionOrInterruptNoAssertUntil(cv, lk, mockClock->now()));
}

class ThreadedOperationDeadlineTests : public OperationDeadlineTests {
public:
    struct WaitTestState {
        void signal() {
            stdx::lock_guard<stdx::mutex> lk(mutex);
            invariant(!isSignaled);
            isSignaled = true;
            cv.notify_all();
        }

        stdx::mutex mutex;
        stdx::condition_variable cv;
        bool isSignaled = false;
    };

    stdx::future<stdx::cv_status> startWaiterWithUntilAndMaxTime(OperationContext* txn,
                                                                 WaitTestState* state,
                                                                 Date_t until,
                                                                 Date_t maxTime) {

        auto barrier = std::make_shared<unittest::Barrier>(2);
        auto task = stdx::packaged_task<stdx::cv_status()>([=] {
            if (maxTime < Date_t::max()) {
                txn->setDeadlineByDate(maxTime);
            }
            auto predicate = [state] { return state->isSignaled; };
            stdx::unique_lock<stdx::mutex> lk(state->mutex);
            barrier->countDownAndWait();
            if (until < Date_t::max()) {
                return txn->waitForConditionOrInterruptUntil(state->cv, lk, until, predicate);
            } else {
                txn->waitForConditionOrInterrupt(state->cv, lk, predicate);
                return stdx::cv_status::no_timeout;
            }
        });
        auto result = task.get_future();
        stdx::thread(std::move(task)).detach();
        barrier->countDownAndWait();

        // Now we know that the waiter task must own the mutex, because it does not signal the
        // barrier until it does.
        stdx::lock_guard<stdx::mutex> lk(state->mutex);

        // Assuming that txn has not already been interrupted and that maxTime and until are
        // unexpired, we know that the waiter must be blocked in the condition variable, because it
        // held the mutex before we tried to acquire it, and only releases it on condition variable
        // wait.
        return result;
    }

    stdx::future<stdx::cv_status> startWaiter(OperationContext* txn, WaitTestState* state) {
        return startWaiterWithUntilAndMaxTime(txn, state, Date_t::max(), Date_t::max());
    }
};

TEST_F(ThreadedOperationDeadlineTests, KillArrivesWhileWaiting) {
    auto txn = client->makeOperationContext();
    WaitTestState state;
    auto waiterResult = startWaiter(txn.get(), &state);
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    {
        stdx::lock_guard<Client> clientLock(*txn->getClient());
        txn->markKilled();
    }
    ASSERT_THROWS_CODE(waiterResult.get(), DBException, ErrorCodes::Interrupted);
}

TEST_F(ThreadedOperationDeadlineTests, MaxTimeExpiresWhileWaiting) {
    auto txn = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(txn.get(),
                                                       &state,
                                                       startDate + Seconds{60},   // until
                                                       startDate + Seconds{10});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{2});
    ASSERT_THROWS_CODE(waiterResult.get(), DBException, ErrorCodes::ExceededTimeLimit);
}

TEST_F(ThreadedOperationDeadlineTests, UntilExpiresWhileWaiting) {
    auto txn = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(txn.get(),
                                                       &state,
                                                       startDate + Seconds{10},   // until
                                                       startDate + Seconds{60});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{2});
    ASSERT(stdx::cv_status::timeout == waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalOne) {
    auto txn = client->makeOperationContext();
    WaitTestState state;
    auto waiterResult = startWaiter(txn.get(), &state);

    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    state.signal();
    ASSERT(stdx::cv_status::no_timeout == waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, KillOneSignalAnother) {
    auto client1 = service->makeClient("client1");
    auto client2 = service->makeClient("client2");
    auto txn1 = client1->makeOperationContext();
    auto txn2 = client2->makeOperationContext();
    WaitTestState state1;
    WaitTestState state2;
    auto waiterResult1 = startWaiter(txn1.get(), &state1);
    auto waiterResult2 = startWaiter(txn2.get(), &state2);
    ASSERT(stdx::future_status::ready !=
           waiterResult1.wait_for(Milliseconds::zero().toSystemDuration()));
    ASSERT(stdx::future_status::ready !=
           waiterResult2.wait_for(Milliseconds::zero().toSystemDuration()));
    {
        stdx::lock_guard<Client> clientLock(*txn1->getClient());
        txn1->markKilled();
    }
    ASSERT_THROWS_CODE(waiterResult1.get(), DBException, ErrorCodes::Interrupted);
    ASSERT(stdx::future_status::ready !=
           waiterResult2.wait_for(Milliseconds::zero().toSystemDuration()));
    state2.signal();
    ASSERT(stdx::cv_status::no_timeout == waiterResult2.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalBeforeUntilExpires) {
    auto txn = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(txn.get(),
                                                       &state,
                                                       startDate + Seconds{10},   // until
                                                       startDate + Seconds{60});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    state.signal();
    ASSERT(stdx::cv_status::no_timeout == waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalBeforeMaxTimeExpires) {
    auto txn = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(txn.get(),
                                                       &state,
                                                       startDate + Seconds{60},   // until
                                                       startDate + Seconds{10});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    state.signal();
    ASSERT(stdx::cv_status::no_timeout == waiterResult.get());
}

}  // namespace

}  // namespace mongo
