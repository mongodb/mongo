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

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Mock clock source that returns a fixed time until explicitly advanced.
 */
class ClockSourceMock final : public ClockSource {
public:
    /**
     * Constructs a ClockSourceMock with the current time set to the Unix epoch.
     */
    ClockSourceMock() {
        _tracksSystemClock = false;
    }

    Milliseconds getPrecision() override;
    Date_t now() override;
    Status setAlarm(Date_t when, stdx::function<void()> action) override;

    /**
     * Advances the current time by the given value.
     */
    void advance(Milliseconds ms);

    /**
     * Resets the current time to the given value.
     */
    void reset(Date_t newNow);

private:
    using Alarm = std::pair<Date_t, stdx::function<void()>>;
    void _processAlarms(stdx::unique_lock<stdx::mutex> lk);

    stdx::mutex _mutex;
    Date_t _now;
    std::vector<Alarm> _alarms;
};

class SharedClockSourceAdapter final : public ClockSource {
public:
    explicit SharedClockSourceAdapter(std::shared_ptr<ClockSource> source)
        : _source(std::move(source)) {
        _tracksSystemClock = _source->tracksSystemClock();
    }

    Milliseconds getPrecision() override {
        return _source->getPrecision();
    }

    Date_t now() override {
        return _source->now();
    }

    Status setAlarm(Date_t when, stdx::function<void()> action) override {
        return _source->setAlarm(when, std::move(action));
    }

private:
    std::shared_ptr<ClockSource> _source;
};

}  // namespace mongo
