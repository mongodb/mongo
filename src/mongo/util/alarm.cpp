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

#include "mongo/platform/basic.h"

#include "mongo/util/alarm.h"

namespace mongo {

class AlarmSchedulerPrecise::HandleImpl final
    : public AlarmScheduler::Handle,
      public std::enable_shared_from_this<AlarmSchedulerPrecise::HandleImpl> {
public:
    HandleImpl(std::weak_ptr<AlarmSchedulerPrecise> service, AlarmSchedulerPrecise::AlarmMapIt it)
        : _service(std::move(service)), _myIt(std::move(it)) {}

    struct MakeEmptyHandle {};
    explicit HandleImpl(MakeEmptyHandle)
        : _service(std::shared_ptr<AlarmSchedulerPrecise>(nullptr)), _myIt(), _done(true) {}

    Status cancel() override {
        auto service = _service.lock();
        if (!service) {
            return {ErrorCodes::ShutdownInProgress, "The alarm scheduler was shutdown"};
        }

        stdx::unique_lock<stdx::mutex> lk(service->_mutex);
        if (_done) {
            return {ErrorCodes::AlarmAlreadyFulfilled, "The alarm has already been canceled"};
        }

        auto state = std::move(_myIt->second);
        service->_alarms.erase(_myIt);
        lk.unlock();

        std::move(state.promise)
            .setError({ErrorCodes::CallbackCanceled,
                       "The alarm was canceled before it expired or could be processed"});
        return Status::OK();
    }

    void setDone() {
        _done = true;
    }

private:
    std::weak_ptr<AlarmSchedulerPrecise> const _service;
    AlarmSchedulerPrecise::AlarmMapIt _myIt;
    bool _done = false;
};

AlarmSchedulerPrecise::~AlarmSchedulerPrecise() {
    clearAllAlarms();
}

AlarmScheduler::Alarm AlarmSchedulerPrecise::alarmAt(Date_t date) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_shutdown) {
        Alarm ret;
        ret.future = Future<void>::makeReady(
            Status(ErrorCodes::ShutdownInProgress, "Alarm scheduler has been shut down."));
        ret.handle = std::make_shared<HandleImpl>(HandleImpl::MakeEmptyHandle{});
        return ret;
    }

    auto pf = makePromiseFuture<void>();
    auto it = _alarms.emplace(date, AlarmData(std::move(pf.promise)));
    auto nextAlarm = _alarms.begin()->first;
    auto ret = std::make_shared<HandleImpl>(shared_from_this(), it);
    it->second.handle = ret;
    lk.unlock();

    callRegisterHook(nextAlarm, shared_from_this());
    return {std::move(pf.future), std::move(ret)};
}

void AlarmSchedulerPrecise::processExpiredAlarms(
    boost::optional<AlarmScheduler::AlarmExpireHook> hook) {
    AlarmCount processed = 0;
    auto now = clockSource()->now();
    std::vector<Promise<void>> toExpire;
    AlarmMapIt it;

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    for (it = _alarms.begin(); it != _alarms.end();) {
        if (hook && !(*hook)(processed + 1)) {
            break;
        }

        if (it->first > now) {
            break;
        }

        processed++;
        toExpire.push_back(std::move(it->second.promise));
        auto handle = it->second.handle.lock();
        if (handle) {
            handle->setDone();
        }

        it = _alarms.erase(it);
    }

    lk.unlock();

    for (auto& promise : toExpire) {
        promise.emplaceValue();
    }
}

Date_t AlarmSchedulerPrecise::nextAlarm() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return (_alarms.empty()) ? Date_t::max() : _alarms.begin()->first;
}

void AlarmSchedulerPrecise::clearAllAlarms() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _clearAllAlarmsImpl(lk);
}

void AlarmSchedulerPrecise::clearAllAlarmsAndShutdown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _shutdown = true;
    _clearAllAlarmsImpl(lk);
}

void AlarmSchedulerPrecise::_clearAllAlarmsImpl(stdx::unique_lock<stdx::mutex>& lk) {
    std::vector<Promise<void>> toExpire;
    for (AlarmMapIt it = _alarms.begin(); it != _alarms.end();) {
        toExpire.push_back(std::move(it->second.promise));
        auto handle = it->second.handle.lock();
        if (handle) {
            handle->setDone();
        }
        it = _alarms.erase(it);
    }

    lk.unlock();
    for (auto& alarm : toExpire) {
        alarm.setError({ErrorCodes::CallbackCanceled, "Alarm scheduler was cleared"});
    }
}

}  // namespace mongo
