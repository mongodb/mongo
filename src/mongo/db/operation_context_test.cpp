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

#include <boost/optional.hpp>

#include "mongo/db/client.h"
#include "mongo/db/json.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_group.h"
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

using unittest::assertGet;

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

TEST(OperationContextTest, NoSessionIdNoTransactionNumber) {
    auto serviceCtx = stdx::make_unique<ServiceContextNoop>();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    ASSERT(!opCtx->getLogicalSessionId());
    ASSERT(!opCtx->getTxnNumber());
}

TEST(OperationContextTest, SessionIdNoTransactionNumber) {
    auto serviceCtx = stdx::make_unique<ServiceContextNoop>();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    const auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);

    ASSERT(opCtx->getLogicalSessionId());
    ASSERT_EQUALS(lsid, *opCtx->getLogicalSessionId());

    ASSERT(!opCtx->getTxnNumber());
}

TEST(OperationContextTest, SessionIdAndTransactionNumber) {
    auto serviceCtx = stdx::make_unique<ServiceContextNoop>();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    const auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(5);

    ASSERT(opCtx->getTxnNumber());
    ASSERT_EQUALS(5, *opCtx->getTxnNumber());
}

TEST(OperationContextTest, OpCtxGroup) {
    OperationContextGroup group1;
    ASSERT_TRUE(group1.isEmpty());
    {
        auto serviceCtx1 = stdx::make_unique<ServiceContextNoop>();
        auto client1 = serviceCtx1->makeClient("OperationContextTest1");
        auto opCtx1 = group1.makeOperationContext(*client1);
        ASSERT_FALSE(group1.isEmpty());

        auto serviceCtx2 = stdx::make_unique<ServiceContextNoop>();
        auto client2 = serviceCtx2->makeClient("OperationContextTest2");
        {
            auto opCtx2 = group1.makeOperationContext(*client2);
            opCtx1.discard();
            ASSERT_FALSE(group1.isEmpty());
        }
        ASSERT_TRUE(group1.isEmpty());

        auto opCtx3 = group1.makeOperationContext(*client1);
        auto opCtx4 = group1.makeOperationContext(*client2);
        ASSERT_TRUE(opCtx3->checkForInterruptNoAssert().isOK());    // use member op->
        ASSERT_TRUE((*opCtx4).checkForInterruptNoAssert().isOK());  // use conversion to OC*
        group1.interrupt(ErrorCodes::InternalError);
        ASSERT_FALSE(opCtx3->checkForInterruptNoAssert().isOK());
        ASSERT_FALSE((*opCtx4).checkForInterruptNoAssert().isOK());

        auto serviceCtx3 = stdx::make_unique<ServiceContextNoop>();
        auto client3 = serviceCtx3->makeClient("OperationContextTest3");
        auto opCtx5 = group1.makeOperationContext(*client3);
        ASSERT_FALSE(opCtx5->checkForInterruptNoAssert().isOK());  // interrupt is sticky
    }
    ASSERT_TRUE(group1.isEmpty());

    {
        group1.resetInterrupt();
        auto serviceCtx1 = stdx::make_unique<ServiceContextNoop>();
        auto client1 = serviceCtx1->makeClient("OperationContextTest3");
        auto opCtx1 = group1.makeOperationContext(*client1);
        ASSERT_TRUE(opCtx1->checkForInterruptNoAssert().isOK());  // interrupt unstuck
    }

    OperationContextGroup group2;
    {
        auto serviceCtx = stdx::make_unique<ServiceContextNoop>();
        auto client = serviceCtx->makeClient("OperationContextTest1");
        auto opCtx2 = group2.adopt(client->makeOperationContext());
        ASSERT_FALSE(group2.isEmpty());
        ASSERT_TRUE(opCtx2->checkForInterruptNoAssert().isOK());
        group2.interrupt(ErrorCodes::InternalError);
        ASSERT_FALSE(opCtx2->checkForInterruptNoAssert().isOK());
        opCtx2.discard();
        ASSERT(opCtx2.opCtx() == nullptr);
        ASSERT_TRUE(group2.isEmpty());
    }

    OperationContextGroup group3;
    OperationContextGroup group4;
    {
        auto serviceCtx = stdx::make_unique<ServiceContextNoop>();
        auto client3 = serviceCtx->makeClient("OperationContextTest3");
        auto opCtx3 = group3.makeOperationContext(*client3);
        auto p3 = opCtx3.opCtx();
        auto opCtx4 = group4.take(std::move(opCtx3));
        ASSERT_EQ(p3, opCtx4.opCtx());
        ASSERT(opCtx3.opCtx() == nullptr);
        ASSERT_TRUE(group3.isEmpty());
        ASSERT_FALSE(group4.isEmpty());
        group3.interrupt(ErrorCodes::InternalError);
        ASSERT_TRUE(opCtx4->checkForInterruptNoAssert().isOK());
        group4.interrupt(ErrorCodes::InternalError);
        ASSERT_FALSE(opCtx4->checkForInterruptNoAssert().isOK());
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
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Seconds{1});
    mockClock->advance(Milliseconds{500});
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    // 1ms before relative deadline reports no interrupt
    mockClock->advance(Milliseconds{499});
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    // Exactly at deadline reports no interrupt, because setDeadlineAfterNowBy adds one clock
    // precision unit to the deadline, to ensure that the deadline does not expire in less than the
    // requested amount of time.
    mockClock->advance(Milliseconds{1});
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    // Since the mock clock's precision is 1ms, at test start + 1001 ms, we expect
    // checkForInterruptNoAssert to return ExceededTimeLimit.
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, opCtx->checkForInterruptNoAssert());

    // Also at times greater than start + 1001ms, we expect checkForInterruptNoAssert to keep
    // returning ExceededTimeLimit.
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, opCtx->checkForInterruptNoAssert());
}

template <typename D>
void assertLargeRelativeDeadlineLikeInfinity(Client& client, D maxTime) {
    auto opCtx = client.makeOperationContext();
    opCtx->setDeadlineAfterNowBy(maxTime);
    ASSERT_FALSE(opCtx->hasDeadline()) << "Tried to set maxTime to " << maxTime;
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
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Nanoseconds::max());
    ASSERT_TRUE(opCtx->hasDeadline());
    ASSERT_EQ(mockClock->now() + mockClock->getPrecision() +
                  duration_cast<Milliseconds>(Nanoseconds::max()),
              opCtx->getDeadline());
}

TEST_F(OperationDeadlineTests, WaitForMaxTimeExpiredCV) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now());
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, opCtx->waitForConditionOrInterruptNoAssert(cv, lk));
}

TEST_F(OperationDeadlineTests, WaitForMaxTimeExpiredCVWithWaitUntilSet) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now());
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_EQ(
        ErrorCodes::ExceededTimeLimit,
        opCtx->waitForConditionOrInterruptNoAssertUntil(cv, lk, mockClock->now() + Seconds{10})
            .getStatus());
}

TEST_F(OperationDeadlineTests, WaitForKilledOpCV) {
    auto opCtx = client->makeOperationContext();
    opCtx->markKilled();
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_EQ(ErrorCodes::Interrupted, opCtx->waitForConditionOrInterruptNoAssert(cv, lk));
}

TEST_F(OperationDeadlineTests, WaitForUntilExpiredCV) {
    auto opCtx = client->makeOperationContext();
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT(stdx::cv_status::timeout ==
           unittest::assertGet(
               opCtx->waitForConditionOrInterruptNoAssertUntil(cv, lk, mockClock->now())));
}

TEST_F(OperationDeadlineTests, WaitForUntilExpiredCVWithMaxTimeSet) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now() + Seconds{10});
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT(stdx::cv_status::timeout ==
           unittest::assertGet(
               opCtx->waitForConditionOrInterruptNoAssertUntil(cv, lk, mockClock->now())));
}

TEST_F(OperationDeadlineTests, WaitForDurationExpired) {
    auto opCtx = client->makeOperationContext();
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_FALSE(opCtx->waitForConditionOrInterruptFor(
        cv, lk, Milliseconds(-1000), []() -> bool { return false; }));
}

TEST_F(OperationDeadlineTests, DuringWaitMaxTimeExpirationDominatesUntilExpiration) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now());
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT(ErrorCodes::ExceededTimeLimit ==
           opCtx->waitForConditionOrInterruptNoAssertUntil(cv, lk, mockClock->now()));
}

class ThreadedOperationDeadlineTests : public OperationDeadlineTests {
public:
    using CvPred = stdx::function<bool()>;
    using WaitFn = stdx::function<bool(
        OperationContext*, stdx::condition_variable&, stdx::unique_lock<stdx::mutex>&, CvPred)>;

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

    stdx::future<bool> startWaiterWithMaxTime(OperationContext* opCtx,
                                              WaitTestState* state,
                                              WaitFn waitFn,
                                              Date_t maxTime) {

        auto barrier = std::make_shared<unittest::Barrier>(2);
        auto task = stdx::packaged_task<bool()>([=] {
            if (maxTime < Date_t::max()) {
                opCtx->setDeadlineByDate(maxTime);
            }
            auto predicate = [state] { return state->isSignaled; };
            stdx::unique_lock<stdx::mutex> lk(state->mutex);
            barrier->countDownAndWait();
            return waitFn(opCtx, state->cv, lk, predicate);
        });
        auto result = task.get_future();
        stdx::thread(std::move(task)).detach();
        barrier->countDownAndWait();

        // Now we know that the waiter task must own the mutex, because it does not signal the
        // barrier until it does.
        stdx::lock_guard<stdx::mutex> lk(state->mutex);

        // Assuming that opCtx has not already been interrupted and that maxTime and until are
        // unexpired, we know that the waiter must be blocked in the condition variable, because it
        // held the mutex before we tried to acquire it, and only releases it on condition variable
        // wait.
        return result;
    }

    stdx::future<bool> startWaiterWithUntilAndMaxTime(OperationContext* opCtx,
                                                      WaitTestState* state,
                                                      Date_t until,
                                                      Date_t maxTime) {
        const auto waitFn = [until](OperationContext* opCtx,
                                    stdx::condition_variable& cv,
                                    stdx::unique_lock<stdx::mutex>& lk,
                                    CvPred predicate) {
            if (until < Date_t::max()) {
                return opCtx->waitForConditionOrInterruptUntil(cv, lk, until, predicate);
            } else {
                opCtx->waitForConditionOrInterrupt(cv, lk, predicate);
                return true;
            }
        };
        return startWaiterWithMaxTime(opCtx, state, waitFn, maxTime);
    }

    template <typename Period>
    stdx::future<bool> startWaiterWithDurationAndMaxTime(OperationContext* opCtx,
                                                         WaitTestState* state,
                                                         Duration<Period> duration,
                                                         Date_t maxTime) {
        const auto waitFn = [duration](OperationContext* opCtx,
                                       stdx::condition_variable& cv,
                                       stdx::unique_lock<stdx::mutex>& lk,
                                       CvPred predicate) {
            return opCtx->waitForConditionOrInterruptFor(cv, lk, duration, predicate);
        };
        return startWaiterWithMaxTime(opCtx, state, waitFn, maxTime);
    }

    stdx::future<bool> startWaiter(OperationContext* opCtx, WaitTestState* state) {
        return startWaiterWithUntilAndMaxTime(opCtx, state, Date_t::max(), Date_t::max());
    }

    stdx::future<bool> startWaiterWithSleepUntilAndMaxTime(OperationContext* opCtx,
                                                           WaitTestState* state,
                                                           Date_t sleepUntil,
                                                           Date_t maxTime) {
        auto waitFn = [sleepUntil](OperationContext* opCtx,
                                   stdx::condition_variable& cv,
                                   stdx::unique_lock<stdx::mutex>& lk,
                                   CvPred predicate) {
            lk.unlock();
            opCtx->sleepUntil(sleepUntil);
            lk.lock();
            return false;
        };
        return startWaiterWithMaxTime(opCtx, state, waitFn, maxTime);
    }

    template <typename Period>
    stdx::future<bool> startWaiterWithSleepForAndMaxTime(OperationContext* opCtx,
                                                         WaitTestState* state,
                                                         Duration<Period> sleepFor,
                                                         Date_t maxTime) {
        auto waitFn = [sleepFor](OperationContext* opCtx,
                                 stdx::condition_variable& cv,
                                 stdx::unique_lock<stdx::mutex>& lk,
                                 CvPred predicate) {
            lk.unlock();
            opCtx->sleepFor(sleepFor);
            lk.lock();
            return false;
        };
        return startWaiterWithMaxTime(opCtx, state, waitFn, maxTime);
    }
};

TEST_F(ThreadedOperationDeadlineTests, KillArrivesWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto waiterResult = startWaiter(opCtx.get(), &state);
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        opCtx->markKilled();
    }
    ASSERT_THROWS_CODE(waiterResult.get(), DBException, ErrorCodes::Interrupted);
}

TEST_F(ThreadedOperationDeadlineTests, MaxTimeExpiresWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(opCtx.get(),
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
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(opCtx.get(),
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
    ASSERT_FALSE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, ForExpiresWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithDurationAndMaxTime(
        opCtx.get(), &state, Seconds{10}, startDate + Seconds{60});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{2});
    ASSERT_FALSE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalOne) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto waiterResult = startWaiter(opCtx.get(), &state);

    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    state.signal();
    ASSERT_TRUE(waiterResult.get());
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
    ASSERT_TRUE(waiterResult2.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalBeforeUntilExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(opCtx.get(),
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
    ASSERT_TRUE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalBeforeMaxTimeExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(opCtx.get(),
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
    ASSERT_TRUE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepUntilWithExpiredUntilDoesNotBlock) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithSleepUntilAndMaxTime(opCtx.get(),
                                                            &state,
                                                            startDate - Seconds{10},   // until
                                                            startDate + Seconds{60});  // maxTime
    ASSERT_FALSE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepUntilExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithSleepUntilAndMaxTime(opCtx.get(),
                                                            &state,
                                                            startDate + Seconds{10},   // until
                                                            startDate + Seconds{60});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{2});
    ASSERT_FALSE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepForWithExpiredForDoesNotBlock) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithSleepForAndMaxTime(
        opCtx.get(), &state, Seconds{-10}, startDate + Seconds{60});  // maxTime
    ASSERT_FALSE(waiterResult.get());
}

}  // namespace

}  // namespace mongo
