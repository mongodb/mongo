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
#pragma once

#include <map>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"

namespace mongo {

/*
 * An alarm scheduler will fill a Future<void> at some time in the future and allow the caller to
 * cancel specific alarms by opaque ID.
 *
 * All alarm service implementations take a ClockSource and all Date_t/Durations used to schedule
 * alarms must be in relation to that ClockSource's now() and epoch.
 *
 * A scheduler won't actually process alarms. Some executor must be calling processExpiredAlarms()
 * in order for alarms to fire.
 */
class AlarmScheduler {
public:
    /*
     * This type gets returned when an alarm is scheduled and allows you to cancel the alarm.
     *
     * Once this handle is destroyed, the alarm will still be processed if it is still outstanding
     * and there will be no way to cancel it.
     */
    class Handle {
    public:
        virtual ~Handle() = default;
        /*
         * Cancels the alarm if it has not already been fulfilled. If the alarm has already been
         * fulfilled this returns an ErrorCodes::AlarmAlreadyFulfilled error
         */
        virtual Status cancel() = 0;
    };

    using SharedHandle = std::shared_ptr<Handle>;
    using AlarmCount = uint64_t;

    explicit AlarmScheduler(ClockSource* clockSource) : _clockSource(clockSource) {}

    virtual ~AlarmScheduler() = default;

    /*
     * Fulfills all outstanding alarms with CallbackCanceled. Memory associated with the scheduler
     * may not be freed until the last outstanding Handle is destroyed, however there should be
     * no broken promises.
     *
     * This will be called implicitly by the destructor.
     */
    virtual void clearAllAlarms() = 0;

    /*
     * Clears all alarms as above, and prevents any new alarms from being scheduled. Calls to
     * alarmAt will return a ready Future with a ShutdownInProgress error code.
     */
    virtual void clearAllAlarmsAndShutdown() = 0;

    struct Alarm {
        Future<void> future;
        SharedHandle handle;
    };
    /*
     * Schedules an alarm some milliseconds from now().
     */
    Alarm alarmFromNow(Milliseconds time) {
        return alarmAt(_clockSource->now() + time);
    };

    /*
     * Schedules an alarm at a specific time on the service's clock source.
     */
    virtual Alarm alarmAt(Date_t time) = 0;

    /*
     * Registers a callback that will be called when a new alarm has been registered.
     *
     * The hook will be called with a Date_t representing the next time an alarm will expire after
     * all internal locks have been released. This can be used to unblock between calling
     * processExpiredAlarms() with a new amount of time to block for.
     */
    using AlarmRegisterHook = unique_function<void(Date_t, const std::shared_ptr<AlarmScheduler>&)>;
    void setAlarmRegisterHook(AlarmRegisterHook onAlarmHook) {
        _registerHook = std::move(onAlarmHook);
    }

    /*
     * Processes all alarms that have expired as of now(). If maxAlarms is not boost::none, then
     * this will only expire that many alarms before returning.
     *
     * Returns the number of alarms that were expired by this call.
     */
    using AlarmExpireHook = unique_function<bool(AlarmCount)>;
    virtual void processExpiredAlarms(boost::optional<AlarmExpireHook> hook = boost::none) = 0;

    /*
     * Returns the Date_t of the next scheduled alarm.
     */
    virtual Date_t nextAlarm() = 0;

    virtual ClockSource* clockSource() const {
        return _clockSource;
    }

protected:
    void callRegisterHook(Date_t nextAlarm, const std::shared_ptr<AlarmScheduler>& which) {
        if (_registerHook) {
            _registerHook(nextAlarm, which);
        }
    }

private:
    AlarmRegisterHook _registerHook;
    ClockSource* const _clockSource;
};

/*
 * Implements a basic alarm scheduler based on a multimap of date_t to promise.
 *
 * Scheduling an alarm takes O(log(n)) where n is the number of outstanding alarms.
 * Canceling an alarm is done in constant time.
 * Processing alarms is done in constant time.
 */
class AlarmSchedulerPrecise : public AlarmScheduler,
                              public std::enable_shared_from_this<AlarmSchedulerPrecise> {
public:
    explicit AlarmSchedulerPrecise(ClockSource* clockSource) : AlarmScheduler(clockSource) {}

    ~AlarmSchedulerPrecise();

    void clearAllAlarms() override;

    void clearAllAlarmsAndShutdown() override;

    Alarm alarmAt(Date_t time) override;

    void processExpiredAlarms(boost::optional<AlarmExpireHook> hook) override;

    Date_t nextAlarm() override;

private:
    class HandleImpl;

    enum AlarmState { kOutstanding, kFulfilled, kAbandoned };
    struct AlarmData {
        explicit AlarmData(Promise<void> promise_) : promise(std::move(promise_)) {}

        std::weak_ptr<HandleImpl> handle;
        Promise<void> promise;
    };

    using AlarmMap = std::multimap<Date_t, AlarmData>;
    using AlarmMapIt = AlarmMap::iterator;

    void _clearAllAlarmsImpl(stdx::unique_lock<stdx::mutex>& lk);

    stdx::mutex _mutex;
    bool _shutdown = false;
    AlarmMap _alarms;
};

}  // namespace
