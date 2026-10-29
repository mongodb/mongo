// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/alarm.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/alarm_runner_background_thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"

#include <type_traits>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
TEST(AlarmScheduler, BasicSingleThread) {
    auto clockSource = std::make_unique<ClockSourceMock>();

    std::shared_ptr<AlarmScheduler> scheduler =
        std::make_shared<AlarmSchedulerPrecise>(clockSource.get());

    auto testStart = clockSource->now();
    auto alarm = scheduler->alarmAt(testStart + Milliseconds(10));
    bool firstTimerExpired = false;
    std::move(alarm.future).getAsync([&](Status status) {
        LOGV2(23071, "First timer expired", "error"_attr = status);
        firstTimerExpired = true;
    });

    alarm = scheduler->alarmAt(testStart + Milliseconds(500));
    bool secondTimerExpired = false;
    std::move(alarm.future).getAsync([&](Status status) {
        LOGV2(23072, "Second timer expired", "error"_attr = status);
        secondTimerExpired = true;
    });

    alarm = scheduler->alarmAt(testStart + Milliseconds(515));
    bool thirdTimerExpired = false;
    std::move(alarm.future).getAsync([&](Status status) {
        LOGV2(23073, "Third timer expired", "error"_attr = status);
        thirdTimerExpired = true;
    });
    auto missingEvent = alarm.handle;

    alarm = scheduler->alarmAt(testStart + Milliseconds(250));

    clockSource->advance(Milliseconds(11));
    scheduler->processExpiredAlarms();
    ASSERT_TRUE(firstTimerExpired);
    ASSERT_FALSE(secondTimerExpired);

    ASSERT_OK(alarm.handle->cancel());
    auto cancelledStatus = std::move(alarm.future).getNoThrow();
    ASSERT_EQ(cancelledStatus.code(), ErrorCodes::CallbackCanceled);


    clockSource->advance(Milliseconds(501));
    scheduler->processExpiredAlarms();
    ASSERT_TRUE(secondTimerExpired);
    ASSERT_FALSE(thirdTimerExpired);

    clockSource->advance(Milliseconds(64));
    scheduler->processExpiredAlarms();
    ASSERT_TRUE(thirdTimerExpired);

    cancelledStatus = missingEvent->cancel();
    ASSERT_EQ(cancelledStatus.code(), ErrorCodes::AlarmAlreadyFulfilled);
    alarm = scheduler->alarmAt(testStart + Hours(5));
    scheduler->clearAllAlarmsAndShutdown();
    cancelledStatus = std::move(alarm.future).getNoThrow();
    ASSERT_EQ(cancelledStatus.code(), ErrorCodes::CallbackCanceled);

    alarm = scheduler->alarmFromNow(Milliseconds{50});
    auto shutdownStatus = alarm.future.getNoThrow();
    ASSERT_EQ(shutdownStatus.code(), ErrorCodes::ShutdownInProgress);
}

TEST(AlarmRunner, BasicTest) {
    auto clockSource = std::make_unique<ClockSourceMock>();
    auto scheduler = std::make_shared<AlarmSchedulerPrecise>(clockSource.get());
    AlarmRunnerBackgroundThread runner({scheduler});
    runner.start();

    auto alarm1 = scheduler->alarmFromNow(Milliseconds(10));
    auto alarm2 = scheduler->alarmFromNow(Milliseconds(20));

    Atomic<bool> future2Filled{false};
    auto pf = makePromiseFuture<void>();
    std::move(alarm2.future)
        .getAsync([&future2Filled, promise = std::move(pf.promise)](Status status) mutable {
            ASSERT_OK(status);
            future2Filled.store(true);
            promise.emplaceValue();
        });

    clockSource->advance(Milliseconds(11));

    ASSERT_OK(alarm1.future.getNoThrow());
    ASSERT_FALSE(future2Filled.load());

    clockSource->advance(Milliseconds(21));
    ASSERT_OK(pf.future.getNoThrow());

    auto alarm3 = scheduler->alarmFromNow(Milliseconds(10));

    ASSERT_OK(alarm3.handle->cancel());

    ASSERT_EQ(alarm3.future.getNoThrow().code(), ErrorCodes::CallbackCanceled);

    auto alarm4 = scheduler->alarmFromNow(Milliseconds(10));
    runner.shutdown();

    ASSERT_EQ(alarm4.future.getNoThrow().code(), ErrorCodes::CallbackCanceled);

    auto alarm5 = scheduler->alarmFromNow(Milliseconds(50));
    ASSERT_EQ(alarm5.future.getNoThrow().code(), ErrorCodes::ShutdownInProgress);
}

TEST(AlarmRunner, SeveralSchedulers) {
    auto clockSource = std::make_unique<ClockSourceMock>();
    auto scheduler1 = std::make_shared<AlarmSchedulerPrecise>(clockSource.get());
    auto scheduler2 = std::make_shared<AlarmSchedulerPrecise>(clockSource.get());

    AlarmRunnerBackgroundThread runner({scheduler1, scheduler2});
    runner.start();

    scheduler1->alarmAt(Date_t::max());
    // Schedule two alarms, the first is just to wake up the runner so that it can try to decide
    // which scheduler to wait for next. The second is the one we actually want to wait for.
    auto alarm1 = scheduler2->alarmFromNow(Milliseconds(1));
    auto alarm2 = scheduler2->alarmFromNow(Milliseconds(20));
    clockSource->advance(Milliseconds(2));
    ASSERT_OK(alarm1.future.getNoThrow());

    // If everything goes well then we should be able to wait on this future without blocking.
    clockSource->advance(Milliseconds(20));
    ASSERT_OK(alarm2.future.getNoThrow());

    runner.shutdown();
}
}  // namespace
}  // namespace mongo
