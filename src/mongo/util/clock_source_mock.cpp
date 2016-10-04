/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/util/clock_source_mock.h"

#include <algorithm>

namespace mongo {

Milliseconds ClockSourceMock::getPrecision() {
    return Milliseconds(1);
}

Date_t ClockSourceMock::now() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _now;
}

void ClockSourceMock::advance(Milliseconds ms) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _now += ms;
    _processAlarms(std::move(lk));
}

void ClockSourceMock::reset(Date_t newNow) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _now = newNow;
    _processAlarms(std::move(lk));
}

Status ClockSourceMock::setAlarm(Date_t when, stdx::function<void()> action) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (when <= _now) {
        lk.unlock();
        action();
        return Status::OK();
    }
    _alarms.emplace_back(std::make_pair(when, std::move(action)));
    return Status::OK();
}

void ClockSourceMock::_processAlarms(stdx::unique_lock<stdx::mutex> lk) {
    using std::swap;
    invariant(lk.owns_lock());
    std::vector<Alarm> readyAlarms;
    std::vector<Alarm>::iterator iter;
    auto alarmIsNotExpired = [&](const Alarm& alarm) { return alarm.first > _now; };
    auto expiredAlarmsBegin = std::partition(_alarms.begin(), _alarms.end(), alarmIsNotExpired);
    std::move(expiredAlarmsBegin, _alarms.end(), std::back_inserter(readyAlarms));
    _alarms.erase(expiredAlarmsBegin, _alarms.end());
    lk.unlock();
    for (const auto& alarm : readyAlarms) {
        alarm.second();
    }
}

}  // namespace mongo
