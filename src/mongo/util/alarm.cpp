// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/alarm.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"

#include <type_traits>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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

        std::unique_lock<std::mutex> lk(service->_mutex);
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
    std::unique_lock<std::mutex> lk(_mutex);
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

    std::unique_lock<std::mutex> lk(_mutex);
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
    std::lock_guard<std::mutex> lk(_mutex);
    return (_alarms.empty()) ? Date_t::max() : _alarms.begin()->first;
}

void AlarmSchedulerPrecise::clearAllAlarms() {
    std::unique_lock<std::mutex> lk(_mutex);
    _clearAllAlarmsImpl(lk);
}

void AlarmSchedulerPrecise::clearAllAlarmsAndShutdown() {
    std::unique_lock<std::mutex> lk(_mutex);
    _shutdown = true;
    _clearAllAlarmsImpl(lk);
}

void AlarmSchedulerPrecise::_clearAllAlarmsImpl(std::unique_lock<std::mutex>& lk) {
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
