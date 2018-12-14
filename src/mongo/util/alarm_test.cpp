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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/stdx/chrono.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/alarm.h"
#include "mongo/util/alarm_runner_background_thread.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/log.h"

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
        log() << "Timer expired: " << status;
        firstTimerExpired = true;
    });

    alarm = scheduler->alarmAt(testStart + Milliseconds(500));
    bool secondTimerExpired = false;
    std::move(alarm.future).getAsync([&](Status status) {
        log() << "Second timer expired: " << status;
        secondTimerExpired = true;
    });

    alarm = scheduler->alarmAt(testStart + Milliseconds(515));
    bool thirdTimerExpired = false;
    std::move(alarm.future).getAsync([&](Status status) {
        log() << "third timer expired: " << status;
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

    AtomicWord<bool> future2Filled{false};
    auto pf = makePromiseFuture<void>();
    std::move(alarm2.future).getAsync([&future2Filled,
                                       promise = std::move(pf.promise) ](Status status) mutable {
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
