/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/db/operation_context.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_group.h"
#include "mongo/db/operation_context_options_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/future.h"  // IWYU pragma: keep
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/future.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ostream>
#include <ratio>
#include <string>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

constexpr auto operator""_sec(unsigned long long n) noexcept {
    return Seconds{static_cast<long long>(n)};
}

using unittest::JoinThread;

class OperationContextTest : public ServiceContextTest {
public:
    using ServiceContextTest::ServiceContextTest;

    auto makeClient(std::string desc = "OperationContextTest",
                    std::shared_ptr<transport::Session> session = nullptr) {
        return getServiceContext()->getService()->makeClient(desc, session);
    }
};

TEST_F(OperationContextTest, NoSessionIdNoTransactionNumber) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    ASSERT(!opCtx->getLogicalSessionId());
    ASSERT(!opCtx->getTxnNumber());
}

TEST_F(OperationContextTest, SessionIdNoTransactionNumber) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    const auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);

    ASSERT(opCtx->getLogicalSessionId());
    ASSERT_EQUALS(lsid, *opCtx->getLogicalSessionId());

    ASSERT(!opCtx->getTxnNumber());
}

TEST_F(OperationContextTest, SessionIdAndTransactionNumber) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    const auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(5);

    ASSERT(opCtx->getTxnNumber());
    ASSERT_EQUALS(5, *opCtx->getTxnNumber());
}

DEATH_TEST_F(OperationContextTest,
             SettingTransactionNumberWithoutSessionIdShouldCrash,
             "invariant") {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    opCtx->setTxnNumber(5);
}

DEATH_TEST_F(OperationContextTest, CallingMarkKillWithExtraInfoCrashes, "invariant") {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    opCtx->markKilled(ErrorCodes::ForTestingErrorExtraInfo);
}

DEATH_TEST_F(OperationContextTest, CallingSetDeadlineWithExtraInfoCrashes, "invariant") {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(Date_t::now(), ErrorCodes::ForTestingErrorExtraInfo);
}

TEST_F(OperationContextTest, CallingMarkKillWithOptionalExtraInfoSucceeds) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    opCtx->markKilled(ErrorCodes::ForTestingOptionalErrorExtraInfo);
}

TEST_F(OperationContextTest, OpCtxGroup) {
    OperationContextGroup group1;
    ASSERT_TRUE(group1.isEmpty());
    {
        auto serviceCtx1 = ServiceContext::make();
        auto client1 = serviceCtx1->getService()->makeClient("OperationContextTest1");
        auto opCtx1 = group1.makeOperationContext(*client1);
        ASSERT_FALSE(group1.isEmpty());

        auto serviceCtx2 = ServiceContext::make();
        auto client2 = serviceCtx2->getService()->makeClient("OperationContextTest2");
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
    }
    ASSERT_TRUE(group1.isEmpty());

    OperationContextGroup group2;
    {
        auto serviceCtx = ServiceContext::make();
        auto client = serviceCtx->getService()->makeClient("OperationContextTest");
        auto opCtx = group2.adopt(client->makeOperationContext());
        ASSERT_FALSE(group2.isEmpty());
        ASSERT_TRUE(opCtx->checkForInterruptNoAssert().isOK());
        group2.interrupt(ErrorCodes::InternalError);
        ASSERT_FALSE(opCtx->checkForInterruptNoAssert().isOK());
        opCtx.discard();
        ASSERT(opCtx.opCtx() == nullptr);
        ASSERT_TRUE(group2.isEmpty());
    }
}

TEST_F(OperationContextTest, IgnoreInterruptsWorks) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    opCtx->markKilled(ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::BadValue);
    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::BadValue);

    opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        ASSERT_OK(opCtx->checkForInterruptNoAssert());
        ASSERT_OK(opCtx->getKillStatus());
    });

    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::BadValue);

    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::BadValue);

    getServiceContext()->setKillAllOperations();

    opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        ASSERT_THROWS_CODE(
            opCtx->checkForInterrupt(), DBException, ErrorCodes::InterruptedAtShutdown);
    });
}

TEST_F(OperationContextTest, setIsExecutingShutdownWorks) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    opCtx->markKilled(ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::BadValue);
    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::BadValue);

    opCtx->setIsExecutingShutdown();

    ASSERT_OK(opCtx->checkForInterruptNoAssert());
    ASSERT_OK(opCtx->getKillStatus());

    getServiceContext()->setKillAllOperations();

    ASSERT_OK(opCtx->checkForInterruptNoAssert());
    ASSERT_OK(opCtx->getKillStatus());
}

TEST_F(OperationContextTest, CancellationTokenIsCanceledWhenMarkKilledIsCalled) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();
    auto cancelToken = opCtx->getCancellationToken();

    // Should not be canceled yet.
    ASSERT_FALSE(cancelToken.isCanceled());

    opCtx->markKilled();

    // Now should be canceled.
    ASSERT_TRUE(cancelToken.isCanceled());
}

TEST_F(OperationContextTest, CancellationTokenIsCancelableAtFirst) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();
    auto cancelToken = opCtx->getCancellationToken();
    ASSERT_TRUE(cancelToken.isCancelable());
}

class OperationTestWithMockClock : public OperationContextTest {
    // This constructor lets us give a variable name to mockClock so we can copy the same
    // shared_ptr into SharedClockSourceAdapters we give to ServiceContext::make and the
    // mockClock member variable.
    explicit OperationTestWithMockClock(std::shared_ptr<ClockSourceMock> mockClock)
        : OperationContextTest(std::make_unique<ScopedGlobalServiceContextForTest>(
              ServiceContext::make(std::make_unique<SharedClockSourceAdapter>(mockClock),
                                   std::make_unique<SharedClockSourceAdapter>(mockClock),
                                   std::make_unique<TickSourceMock<>>()))),
          mockClock(std::move(mockClock)) {}

public:
    OperationTestWithMockClock()
        : OperationTestWithMockClock(std::make_shared<ClockSourceMock>()) {}

    void setUp() override {
        client = getServiceContext()->getService()->makeClient("OperationDeadlineTest");
    }

    TickSourceMock<>* mockTickSource() {
        return checked_cast<TickSourceMock<>*>(getServiceContext()->getTickSource());
    }

    void checkForInterruptForTimeout(OperationContext* opCtx) {
        stdx::mutex m;
        stdx::condition_variable cv;
        stdx::unique_lock<stdx::mutex> lk(m);
        opCtx->waitForConditionOrInterrupt(cv, lk, [] { return false; });
    }

    const std::shared_ptr<ClockSourceMock> mockClock;
    ServiceContext::UniqueClient client;
};
using OperationDeadlineTests = OperationTestWithMockClock;

TEST_F(OperationDeadlineTests, OperationDeadlineExpiration) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Seconds{1}, ErrorCodes::ExceededTimeLimit);
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

TEST_F(OperationDeadlineTests, CancellationTokenIsCanceledAfterDeadlineExpires) {
    auto opCtx = client->makeOperationContext();
    const Seconds timeout{1};
    opCtx->setDeadlineAfterNowBy(timeout, ErrorCodes::ExceededTimeLimit);

    auto cancelToken = opCtx->getCancellationToken();

    // Should not be canceled yet.
    ASSERT_FALSE(cancelToken.isCanceled());

    // Advance past the timeout.
    mockClock->advance(timeout * 2);

    // This is required for the OperationContext to realize that the timeout has passed and mark
    // itself killed, which is what triggers cancellation.
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, opCtx->checkForInterruptNoAssert());

    // Should be canceled now.
    ASSERT_TRUE(cancelToken.isCanceled());
}

TEST_F(OperationDeadlineTests,
       WaitingOnAFutureWithAnOperationContextThatHasCancellationCallbacksDoesNotDeadlock) {
    auto opCtx = client->makeOperationContext();
    const Seconds timeout{1};
    opCtx->setDeadlineAfterNowBy(timeout, ErrorCodes::ExceededTimeLimit);

    auto cancelToken = opCtx->getCancellationToken();

    // Should not be canceled yet.
    ASSERT_FALSE(cancelToken.isCanceled());

    // Advance past the timeout.
    mockClock->advance(timeout * 2);

    // Chain a callback to the token. This will mean that calling cancel() on the CancellationSource
    // will eventually have to acquire a mutex when fulfilling its SharedPromie.
    auto fut = cancelToken.onCancel().unsafeToInlineFuture().then([] {});

    // Make sure this does not deadlock. (Because in a previous implementation, it did.)
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, std::move(fut).waitNoThrow(opCtx.get()));

    // Should be canceled now.
    ASSERT_TRUE(cancelToken.isCanceled());
}

TEST_F(OperationDeadlineTests, InterruptLatency) {
    auto tickSource = checked_cast<TickSourceMock<>*>(getServiceContext()->getTickSource());
    auto opCtx = client->makeOperationContext();

    tickSource->advance(Milliseconds(5));
    ASSERT_EQ(0, opCtx->getKillTime());

    opCtx->markKilled();
    ASSERT_NE(opCtx->getKillTime(), 0);
    ASSERT_EQ(tickSource->getTicks(), opCtx->getKillTime());
    tickSource->advance(Milliseconds(8));

    ASSERT_EQ(tickSource->ticksTo<Milliseconds>(tickSource->getTicks() - opCtx->getKillTime()),
              Milliseconds(8));
}

template <typename D>
void assertLargeRelativeDeadlineLikeInfinity(Client& client, D maxTime) {
    auto opCtx = client.makeOperationContext();
    opCtx->setDeadlineAfterNowBy(maxTime, ErrorCodes::ExceededTimeLimit);
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
    opCtx->setDeadlineAfterNowBy(Nanoseconds::max(), ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->hasDeadline());
    ASSERT_EQ(mockClock->now() + mockClock->getPrecision() +
                  duration_cast<Milliseconds>(Nanoseconds::max()),
              opCtx->getDeadline());
}

TEST_F(OperationDeadlineTests, WaitForMaxTimeExpiredCV) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now(), ErrorCodes::ExceededTimeLimit);
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    ASSERT_THROWS_CODE(opCtx->waitForConditionOrInterrupt(cv, lk, [] { return false; }),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(OperationDeadlineTests, WaitForMaxTimeExpiredCVWithWaitUntilSet) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now(), ErrorCodes::ExceededTimeLimit);
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    ASSERT_THROWS_CODE(opCtx->waitForConditionOrInterruptUntil(
                           cv, lk, mockClock->now() + Seconds{10}, [] { return false; }),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(OperationDeadlineTests, NestedTimeoutsTimeoutInOrder) {
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(mockClock->now() + Milliseconds(500), ErrorCodes::MaxTimeMSExpired);

    bool reachedA = false;
    bool reachedB = false;
    bool reachedC = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());

                try {
                    opCtx->runWithDeadline(
                        mockClock->now() + Milliseconds(50), ErrorCodes::ExceededTimeLimit, [&] {
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            try {
                                opCtx->runWithDeadline(mockClock->now() + Milliseconds(10),
                                                       ErrorCodes::ExceededTimeLimit,
                                                       [&] {
                                                           ASSERT_OK(
                                                               opCtx->checkForInterruptNoAssert());
                                                           ASSERT_OK(opCtx->getKillStatus());
                                                           mockClock->advance(Milliseconds(20));
                                                           checkForInterruptForTimeout(opCtx.get());
                                                           ASSERT(false);
                                                       });
                            } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                                opCtx->checkForInterrupt();
                                ASSERT_OK(opCtx->getKillStatus());
                                mockClock->advance(Milliseconds(50));
                                reachedA = true;
                            }

                            opCtx->checkForInterrupt();
                        });
                } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                    opCtx->checkForInterrupt();
                    ASSERT_OK(opCtx->getKillStatus());
                    mockClock->advance(Milliseconds(50));
                    reachedB = true;
                }

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        reachedC = true;
        ASSERT_OK(opCtx->getKillStatus());
        ASSERT_OK(opCtx->checkForInterruptNoAssert());
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT(reachedC);

    ASSERT_OK(opCtx->getKillStatus());

    mockClock->advance(Seconds(1));

    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::MaxTimeMSExpired);
}

TEST_F(OperationDeadlineTests, NestedTimeoutsThatViolateMaxTime) {
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(mockClock->now() + Milliseconds(10), ErrorCodes::MaxTimeMSExpired);

    bool reachedA = false;
    bool reachedB = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());
                try {
                    opCtx->runWithDeadline(
                        mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            ASSERT_OK(opCtx->getKillStatus());
                            mockClock->advance(Milliseconds(50));
                            opCtx->checkForInterrupt();
                        });
                } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                    reachedA = true;
                }

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::MaxTimeMSExpired>&) {
        reachedB = true;
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
}

TEST_F(OperationDeadlineTests, NestedNonMaxTimeMSTimeoutsThatAreLargerAreIgnored) {
    auto opCtx = client->makeOperationContext();

    bool reachedA = false;
    bool reachedB = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(10), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());
                try {
                    opCtx->runWithDeadline(
                        mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            ASSERT_OK(opCtx->getKillStatus());
                            mockClock->advance(Milliseconds(50));
                            opCtx->checkForInterrupt();
                        });
                } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                    reachedA = true;
                }

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        reachedB = true;
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
}

TEST_F(OperationDeadlineTests, DeadlineAfterIgnoreInterruptsReopens) {
    auto opCtx = client->makeOperationContext();

    bool reachedA = false;
    bool reachedB = false;
    bool reachedC = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(500), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());

                opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
                    try {
                        opCtx->runWithDeadline(
                            mockClock->now() + Seconds(1), ErrorCodes::ExceededTimeLimit, [&] {
                                ASSERT_OK(opCtx->checkForInterruptNoAssert());
                                ASSERT_OK(opCtx->getKillStatus());
                                mockClock->advance(Milliseconds(750));
                                ASSERT_OK(opCtx->checkForInterruptNoAssert());
                                mockClock->advance(Milliseconds(500));
                                reachedA = true;
                                opCtx->checkForInterrupt();
                            });
                    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                        opCtx->checkForInterrupt();
                        reachedB = true;
                    }
                });

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        reachedC = true;
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT(reachedC);
}

TEST_F(OperationDeadlineTests, DeadlineAfterSetIsExecutingShutdownReopens) {
    auto opCtx = client->makeOperationContext();

    bool reachedA = false;
    bool reachedB = false;
    bool reachedC = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(500), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());

                opCtx->setIsExecutingShutdown();
                try {
                    opCtx->runWithDeadline(
                        mockClock->now() + Seconds(1), ErrorCodes::ExceededTimeLimit, [&] {
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            ASSERT_OK(opCtx->getKillStatus());
                            mockClock->advance(Milliseconds(750));
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            mockClock->advance(Milliseconds(500));
                            reachedA = true;
                            opCtx->checkForInterrupt();
                        });
                } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                    opCtx->checkForInterrupt();
                    reachedB = true;
                }

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        reachedC = true;
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT_FALSE(reachedC);
}

TEST_F(OperationDeadlineTests, DeadlineAfterRunWithoutInterruptSeesViolatedMaxMS) {
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(mockClock->now() + Milliseconds(100), ErrorCodes::MaxTimeMSExpired);

    ASSERT_THROWS_CODE(opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(200), ErrorCodes::ExceededTimeLimit, [&] {
                mockClock->advance(Milliseconds(300));
                opCtx->checkForInterrupt();
            });
    }),
                       DBException,
                       ErrorCodes::MaxTimeMSExpired);
}

TEST_F(OperationDeadlineTests, DeadlineAfterRunWithoutInterruptDoesntSeeUnviolatedMaxMS) {
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(mockClock->now() + Milliseconds(200), ErrorCodes::MaxTimeMSExpired);

    ASSERT_THROWS_CODE(opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                mockClock->advance(Milliseconds(150));
                opCtx->checkForInterrupt();
            });
    }),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);
}

TEST_F(OperationDeadlineTests, WaitForKilledOpCV) {
    auto opCtx = client->makeOperationContext();
    opCtx->markKilled();
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_THROWS_CODE(opCtx->waitForConditionOrInterrupt(cv, lk, [] { return false; }),
                       DBException,
                       ErrorCodes::Interrupted);
}

TEST_F(OperationDeadlineTests, WaitForUntilExpiredCV) {
    auto opCtx = client->makeOperationContext();
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_FALSE(
        opCtx->waitForConditionOrInterruptUntil(cv, lk, mockClock->now(), [] { return false; }));
}

TEST_F(OperationDeadlineTests, WaitForUntilExpiredCVWithMaxTimeSet) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now() + Seconds{10}, ErrorCodes::ExceededTimeLimit);
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_FALSE(
        opCtx->waitForConditionOrInterruptUntil(cv, lk, mockClock->now(), [] { return false; }));
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
    opCtx->setDeadlineByDate(mockClock->now(), ErrorCodes::ExceededTimeLimit);
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    ASSERT_THROWS_CODE(
        opCtx->waitForConditionOrInterruptUntil(cv, lk, mockClock->now(), [] { return false; }),
        DBException,
        ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(OperationDeadlineTests, MaxTimeRestoredAfterDeadlineGuard) {
    auto tickSource = checked_cast<TickSourceMock<>*>(getServiceContext()->getTickSource());
    auto opCtx = client->makeOperationContext();
    auto originDeadline = mockClock->now() + Seconds{1};
    opCtx->setDeadlineByDate(originDeadline, ErrorCodes::MaxTimeMSExpired);

    ASSERT_EQ(Seconds{1}, opCtx->getRemainingMaxTimeMicros());

    auto newDeadline = mockClock->now() + Milliseconds{500};
    opCtx->runWithDeadline(newDeadline, ErrorCodes::MaxTimeMSExpired, [&]() -> void {
        tickSource->advance(Milliseconds(300));
        mockClock->advance(Milliseconds(300));
    });

    ASSERT_EQ(originDeadline, opCtx->getDeadline());
    ASSERT_EQ(Milliseconds{700}, opCtx->getRemainingMaxTimeMicros());
}

class ThreadedOperationDeadlineTests : public OperationDeadlineTests {
public:
    using CvPred = std::function<bool()>;
    using WaitFn = std::function<bool(
        OperationContext*, stdx::condition_variable&, stdx::unique_lock<stdx::mutex>&, CvPred)>;

    struct WaitTestState {
        void signal() {
            stdx::lock_guard<stdx::mutex> lk(mutex);
            invariant(!isSignaled);
            isSignaled = true;
            cv.notify_all();
        }

        stdx::future<bool> start(OperationContext* opCtx,
                                 boost::optional<Date_t> maxTime,
                                 WaitFn waitFn) {
            auto barrier = std::make_shared<unittest::Barrier>(2);
            task = std::packaged_task<bool()>([=, this] {  // NOLINT
                if (maxTime)
                    opCtx->setDeadlineByDate(*maxTime, ErrorCodes::ExceededTimeLimit);
                stdx::unique_lock<stdx::mutex> lk(mutex);
                barrier->countDownAndWait();
                return waitFn(opCtx, cv, lk, [&] { return isSignaled; });
            });
            auto result = task.get_future();
            waiter = JoinThread([this] { task(); });
            barrier->countDownAndWait();

            // Now we know that the waiter task must own the mutex, because it does not signal the
            // barrier until it does.
            stdx::lock_guard<stdx::mutex> lk(mutex);

            // Assuming that opCtx has not already been interrupted and that maxTime and until are
            // unexpired, we know that the waiter must be blocked in the condition variable, because
            // it held the mutex before we tried to acquire it, and only releases it on condition
            // variable wait.
            return result;
        }

        stdx::mutex mutex;
        stdx::condition_variable cv;
        bool isSignaled = false;
        std::packaged_task<bool()> task;  // NOLINT
        JoinThread waiter;
    };

    static WaitFn waitFn() {
        return [](OperationContext* opCtx,
                  stdx::condition_variable& cv,
                  stdx::unique_lock<stdx::mutex>& lk,
                  CvPred predicate) {
            opCtx->waitForConditionOrInterrupt(cv, lk, predicate);
            return true;
        };
    }

    static WaitFn waitUntilFn(Date_t until) {
        return [until](OperationContext* opCtx,
                       stdx::condition_variable& cv,
                       stdx::unique_lock<stdx::mutex>& lk,
                       CvPred predicate) {
            return opCtx->waitForConditionOrInterruptUntil(cv, lk, until, predicate);
        };
    }

    template <typename Period>
    static WaitFn waitDurationFn(Duration<Period> duration) {
        return [duration](OperationContext* opCtx,
                          stdx::condition_variable& cv,
                          stdx::unique_lock<stdx::mutex>& lk,
                          CvPred predicate) {
            return opCtx->waitForConditionOrInterruptFor(cv, lk, duration, predicate);
        };
    }

    static WaitFn sleepUntilFn(Date_t sleepUntil) {
        return [sleepUntil](OperationContext* opCtx,
                            stdx::condition_variable& cv,
                            stdx::unique_lock<stdx::mutex>& lk,
                            CvPred predicate) {
            lk.unlock();
            opCtx->sleepUntil(sleepUntil);
            lk.lock();
            return false;
        };
    }

    template <typename Period>
    static WaitFn sleepForFn(Duration<Period> sleepFor) {
        return [sleepFor](OperationContext* opCtx,
                          stdx::condition_variable& cv,
                          stdx::unique_lock<stdx::mutex>& lk,
                          CvPred predicate) {
            lk.unlock();
            opCtx->sleepFor(sleepFor);
            lk.lock();
            return false;
        };
    }

    template <typename T>
    static bool isReady(const stdx::future<T>& fut) {
        return fut.wait_for((0_sec).toSystemDuration()) == stdx::future_status::ready;
    }
};

TEST_F(ThreadedOperationDeadlineTests, KillArrivesWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto fut = state.start(&*opCtx, {}, waitFn());
    ASSERT_FALSE(isReady(fut));
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        opCtx->markKilled();
    }
    ASSERT_THROWS_CODE(fut.get(), DBException, ErrorCodes::Interrupted);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(ThreadedOperationDeadlineTests, MaxTimeExpiresWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto t0 = mockClock->now();
    auto fut = state.start(&*opCtx, t0 + 10_sec, waitUntilFn(t0 + 60_sec));
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(9_sec);
    ASSERT_FALSE(isReady(fut));
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    mockClock->advance(2_sec);
    ASSERT_THROWS_CODE(fut.get(), DBException, ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(ThreadedOperationDeadlineTests, MaxTimeExpiresWhileWaitingForever) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto fut = state.start(&*opCtx, mockClock->now() + 10_sec, waitFn());
    mockClock->advance(11_sec);
    ASSERT_THROWS_CODE(fut.get(), DBException, ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(ThreadedOperationDeadlineTests, UntilExpiresWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto t0 = mockClock->now();
    auto fut = state.start(&*opCtx, t0 + 60_sec, waitUntilFn(t0 + 10_sec));
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(9_sec);
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(2_sec);
    ASSERT_FALSE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, UntilExpiresWhileWaitingWithoutDeadline) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto fut = state.start(&*opCtx, {}, waitUntilFn(mockClock->now() + 10_sec));
    mockClock->advance(11_sec);
    ASSERT_FALSE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, ForExpiresWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto t0 = mockClock->now();
    auto fut = state.start(&*opCtx, t0 + 60_sec, waitDurationFn(10_sec));
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(9_sec);
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(2_sec);
    ASSERT_FALSE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, ForExpiresWhileWaitingWithoutDeadline) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto fut = state.start(&*opCtx, {}, waitDurationFn(10_sec));
    mockClock->advance(11_sec);
    ASSERT_FALSE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalOne) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto fut = state.start(&*opCtx, {}, waitFn());
    ASSERT_FALSE(isReady(fut));
    state.signal();
    ASSERT_TRUE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, KillOneSignalAnother) {
    auto client1 = makeClient("client1");
    auto client2 = makeClient("client2");
    auto txn1 = client1->makeOperationContext();
    auto txn2 = client2->makeOperationContext();
    WaitTestState state1;
    WaitTestState state2;
    auto fut1 = state1.start(txn1.get(), {}, waitFn());
    auto fut2 = state2.start(txn2.get(), {}, waitFn());
    ASSERT_FALSE(isReady(fut1));
    ASSERT_FALSE(isReady(fut2));
    {
        stdx::lock_guard<Client> clientLock(*txn1->getClient());
        txn1->markKilled();
    }
    ASSERT_THROWS_CODE(fut1.get(), DBException, ErrorCodes::Interrupted);
    ASSERT_FALSE(isReady(fut2));
    state2.signal();
    ASSERT_TRUE(fut2.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalBeforeUntilExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto t0 = mockClock->now();
    auto fut = state.start(&*opCtx, t0 + 60_sec, waitUntilFn(t0 + 10_sec));
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(9_sec);
    ASSERT_FALSE(isReady(fut));
    state.signal();
    ASSERT_TRUE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalBeforeMaxTimeExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto t0 = mockClock->now();
    auto fut = state.start(&*opCtx, t0 + 10_sec, waitUntilFn(t0 + 60_sec));
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(9_sec);
    ASSERT_FALSE(isReady(fut));
    state.signal();
    ASSERT_TRUE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepUntilWithExpiredUntilDoesNotBlock) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto t0 = mockClock->now();
    auto fut = state.start(&*opCtx, t0 + 60_sec, sleepUntilFn(t0 - 10_sec));
    ASSERT_FALSE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepUntilExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto t0 = mockClock->now();
    auto fut = state.start(&*opCtx, t0 + 60_sec, sleepUntilFn(t0 + 10_sec));
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(9_sec);
    ASSERT_FALSE(isReady(fut));
    mockClock->advance(2_sec);
    ASSERT_FALSE(fut.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepForWithExpiredForDoesNotBlock) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto t0 = mockClock->now();
    auto fut = state.start(&*opCtx, t0 + 60_sec, sleepForFn(-10_sec));
    ASSERT_FALSE(fut.get());
}

TEST_F(OperationContextTest, TestWaitForConditionOrInterruptUntilAPI) {
    // `waitForConditionOrInterruptUntil` can have four outcomes:
    //
    // 1) The condition is satisfied before any timeouts.
    // 2) The explicit `deadline` function argument is triggered.
    // 3) The operation context implicitly times out, or is interrupted from a killOp command or
    //    shutdown, etc.
    // 4) The deadline supplied may overflow the conversion to the system clock's resolution, from
    //    milliseconds to nanoseconds. This will not cancel the opCtx.
    //
    // Case (1) must return true.
    // Case (2) must return false.
    // Case (3) must throw a MaxTimeMSExpired DBException.
    // Case (4) must throw a DurationOverflow DBException.
    //
    // Case (1) is the hardest to test. The condition variable must be notified by a second thread
    // when the client is waiting on it. Case (1) is also the least in need of having the API
    // tested, thus it's omitted from being tested here.
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    stdx::mutex mutex;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(mutex);

    // Case (2). Expect a Status::OK with a cv_status::timeout.
    Date_t deadline = Date_t::now() + Milliseconds(500);
    ASSERT_EQ(opCtx->waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }),
              false);
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());

    // Case (3). Expect an error of `MaxTimeMSExpired`.
    opCtx->setDeadlineByDate(Date_t::now(), ErrorCodes::MaxTimeMSExpired);
    deadline = Date_t::now() + Seconds(500);
    ASSERT_THROWS_CODE(
        opCtx->waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }),
        DBException,
        ErrorCodes::MaxTimeMSExpired);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());

    // Case (4). Expect an error of 'DurationOverflow'.
    auto secondClient = makeClient();
    auto secondOpCtx = secondClient->makeOperationContext();
    deadline = Date_t::max() - Milliseconds(1);
    ASSERT_THROWS_CODE(
        secondOpCtx->waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }),
        DBException,
        ErrorCodes::DurationOverflow);
    ASSERT_FALSE(secondOpCtx->getCancellationToken().isCanceled());
}

TEST_F(OperationContextTest, TestIsWaitingForConditionOrInterrupt) {
    auto client = makeClient();
    auto optCtx = client->makeOperationContext();

    // Case (1) must return false (immediately after initialization)
    ASSERT_FALSE(optCtx->isWaitingForConditionOrInterrupt());

    // Case (2) must return true while waiting for the condition

    unittest::Barrier barrier(2);

    stdx::thread worker([&] {
        stdx::mutex mutex;
        stdx::condition_variable cv;
        stdx::unique_lock<stdx::mutex> lk(mutex);
        Date_t deadline = Date_t::now() + Milliseconds(300);
        optCtx->waitForConditionOrInterruptUntil(cv, lk, deadline, [&, i = 0]() mutable {
            if (i++ == 0) {
                barrier.countDownAndWait();
            }
            return false;
        });
    });

    barrier.countDownAndWait();
    ASSERT_TRUE(optCtx->isWaitingForConditionOrInterrupt());

    worker.join();
    ASSERT_FALSE(optCtx->isWaitingForConditionOrInterrupt());
}

TEST_F(OperationContextTest, CurrentOpExcludesKilledOperations) {
    auto client = getService()->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();
    const auto expCtx = ExpressionContextBuilder{}
                            .opCtx(opCtx.get())
                            .ns(NamespaceString::createNamespaceString_forTest("foo.bar"_sd))
                            .build();
    for (auto truncateOps : {true, false}) {
        BSONObjBuilder bobNoOpCtx, bobKilledOpCtx;
        // We use a separate client thread to generate CurrentOp reports in presence and absence
        // of an `opCtx`. This is because `CurOp::reportCurrentOpForClient()` accepts an `opCtx`
        // as input and requires it to be present throughout its execution.
        stdx::thread thread([&]() mutable {
            stdx::lock_guard<Client> lk(*opCtx->getClient());

            auto threadClient = getService()->makeClient("ThreadClient");

            // Generate report in absence of any opCtx
            CurOp::reportCurrentOpForClient(expCtx, threadClient.get(), truncateOps, &bobNoOpCtx);

            auto threadOpCtx = threadClient->makeOperationContext();
            getServiceContext()->killAndDelistOperation(threadOpCtx.get());

            // Generate report in presence of a killed opCtx
            CurOp::reportCurrentOpForClient(
                expCtx, threadClient.get(), truncateOps, &bobKilledOpCtx);
        });

        thread.join();
        auto objNoOpCtx = bobNoOpCtx.obj();
        auto objKilledOpCtx = bobKilledOpCtx.obj();

        LOGV2_DEBUG(4780201, 1, "With no opCtx", "object"_attr = objNoOpCtx);
        LOGV2_DEBUG(4780202, 1, "With killed opCtx", "object"_attr = objKilledOpCtx);

        ASSERT_EQ(objNoOpCtx.nFields(), objKilledOpCtx.nFields());

        auto compareBSONObjs = [](BSONObj& a, BSONObj& b) -> bool {
            return (a == b).type == BSONObj::DeferredComparison::Type::kEQ;
        };
        ASSERT(compareBSONObjs(objNoOpCtx, objKilledOpCtx));
    }
}

TEST_F(OperationTestWithMockClock, InterruptCheckOnTime) {
    RAIIServerParameterControllerForTest enableDelinquentTracking(
        "featureFlagRecordDelinquentMetrics", true);
    RAIIServerParameterControllerForTest alwaysTrackInterrupts("overdueInterruptCheckSamplingRate",
                                                               1);
    auto client = getService()->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();
    opCtx->trackOverdueInterruptChecks(mockTickSource()->getTicks());

    opCtx->checkForInterrupt();
    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_EQ(opCtx->numInterruptChecks(), 1);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 0);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{0});
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), Milliseconds{0});
    }

    mockTickSource()->advance(Milliseconds(1));

    opCtx->checkForInterrupt();
    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_EQ(opCtx->numInterruptChecks(), 2);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 0);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{0});
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), Milliseconds{0});
    }
}

TEST_F(OperationTestWithMockClock, InfrequentInterruptChecks) {
    RAIIServerParameterControllerForTest enableDelinquentTracking(
        "featureFlagRecordDelinquentMetrics", true);
    RAIIServerParameterControllerForTest alwaysTrackInterrupts("overdueInterruptCheckSamplingRate",
                                                               1);
    auto client = getService()->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();
    opCtx->trackOverdueInterruptChecks(mockTickSource()->getTicks());

    opCtx->checkForInterrupt();
    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_EQ(opCtx->numInterruptChecks(), 1);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 0);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{0});
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), Milliseconds{0});
    }

    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
    mockTickSource()->advance(5 * interval);

    opCtx->checkForInterrupt();
    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_EQ(opCtx->numInterruptChecks(), 2);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), 4 * interval);
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), 4 * interval);
    }
}

TEST_F(OperationTestWithMockClock, FirstInterruptCheckOverdue) {
    // Create an OperationContext that does not check for interrupt for awhile after creation.
    auto client = getService()->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();
    opCtx->trackOverdueInterruptChecks(mockTickSource()->getTicks());

    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
    mockTickSource()->advance(interval * 5);

    opCtx->checkForInterrupt();
    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);

        ASSERT_EQ(opCtx->numInterruptChecks(), 1);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), 4 * interval);
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), 4 * interval);
    }
}

TEST_F(OperationTestWithMockClock, NeverChecksForInterrupt) {
    auto client = getService()->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();
    opCtx->trackOverdueInterruptChecks(mockTickSource()->getTicks());

    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
    mockTickSource()->advance(interval * 5);

    // Simulate the operation "completing" without ever having checked for interrupt by using
    // updateOverdueInterruptCheckCounters().
    opCtx->updateInterruptCheckCounters();
    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_EQ(opCtx->numInterruptChecks(), 0);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), 4 * interval);
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), 4 * interval);
    }
}

TEST_F(OperationTestWithMockClock, KilledOperationWithOverdueInterruptCheck) {
    // Create an OperationContext that does not check for interrupt for awhile after creation.
    auto client = getService()->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();
    opCtx->trackOverdueInterruptChecks(mockTickSource()->getTicks());

    opCtx->markKilled(ErrorCodes::BadValue);

    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
    mockTickSource()->advance(interval * 5);

    auto killStatus = opCtx->checkForInterruptNoAssert();
    ASSERT_EQUALS(killStatus, ErrorCodes::BadValue);

    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_EQ(opCtx->numInterruptChecks(), 1);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), 4 * interval);
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), 4 * interval);
    }
}

TEST_F(OperationTestWithMockClock, RunWithoutInterruptNotMarkedOverdue) {
    auto client = getService()->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();
    opCtx->trackOverdueInterruptChecks(mockTickSource()->getTicks());

    opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
        mockTickSource()->advance(interval * 5);

        ASSERT_OK(opCtx->checkForInterruptNoAssert());
        ASSERT_OK(opCtx->getKillStatus());

        {
            auto stats = opCtx->overdueInterruptCheckStats();
            ASSERT(stats);
            ASSERT_EQ(opCtx->numInterruptChecks(), 1);
            ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 0);
            ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{0});
            ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), Milliseconds{0});
        }
    });

    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_EQ(opCtx->numInterruptChecks(), 1);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 0);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{0});
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), Milliseconds{0});
    }
}


using OverdueInterruptTestWithInterruptibleWait = ThreadedOperationDeadlineTests;
TEST_F(OverdueInterruptTestWithInterruptibleWait, InterruptibleSleepNotMarkedOverdue) {
    auto opCtx = client->makeOperationContext();
    opCtx->trackOverdueInterruptChecks(mockTickSource()->getTicks());
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
    WaitTestState state;
    auto fut = state.start(&*opCtx, {}, waitDurationFn(interval * 5));
    mockTickSource()->advance(interval * 5 + Milliseconds{1});
    mockClock->advance(interval * 5 + Milliseconds{1});
    ASSERT_FALSE(fut.get());

    // An interrupt check here should not be considered overdue even though a long time
    // passed since the last one. The operation was in an interruptible sleep, so this
    // interrupt check is not overdue.
    ASSERT_OK(opCtx->checkForInterruptNoAssert());


    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_GTE(opCtx->numInterruptChecks(), 2);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 0);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{0});
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), Milliseconds{0});
    }

    // After the thread wakes up from the sleep, the overdue "timer" resets. The thread should be
    // able to go nearly the entire interval without checking for interrupt and not be considered
    // overdue.
    mockTickSource()->advance(interval - Milliseconds{1});
    ASSERT_OK(opCtx->checkForInterruptNoAssert());
    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_GTE(opCtx->numInterruptChecks(), 2);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 0);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{0});
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), Milliseconds{0});
    }
}

TEST_F(OverdueInterruptTestWithInterruptibleWait, KillDuringInterruptibleSleepNotMarkedOverdue) {
    auto client = getService()->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();
    opCtx->trackOverdueInterruptChecks(mockTickSource()->getTicks());

    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};

    // Start a thread which will be killed during an interruptible wait that is longer than the
    // interrupt period.
    auto task = stdx::packaged_task<void()>(
        [&] { WaitTestState{}.start(&*opCtx, {}, waitDurationFn(interval * 5)).get(); });

    auto result = task.get_future();
    JoinThread thread([&task] { task(); });

    // Advance the clock by an amount shorter than the second thread's wait time, and then kill the
    // opCtx.
    mockTickSource()->advance(interval * 2);

    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        opCtx->markKilled();
    }
    thread.join();

    ASSERT_THROWS_CODE(result.get(), DBException, ErrorCodes::Interrupted);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());

    // The opCtx should not be considered overdue. Even though it did go longer than the
    // expected checkForInterrupt interval without calling checkForInterrupt, it was in an
    // interruptible wait.
    {
        auto stats = opCtx->overdueInterruptCheckStats();
        ASSERT(stats);
        ASSERT_GT(opCtx->numInterruptChecks(), 0);
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 0);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{0});
        ASSERT_EQ(stats->overdueMaxTime.loadRelaxed(), Milliseconds{0});
    }
}
}  // namespace
}  // namespace mongo
