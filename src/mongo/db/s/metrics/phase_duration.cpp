/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/metrics/phase_duration.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
static constexpr auto kNoDate = Date_t::min();
boost::optional<Date_t> readAtomicDate(const AtomicWord<Date_t>& date) {
    auto value = date.load();
    if (value == kNoDate) {
        return boost::none;
    }
    return value;
}
}  // namespace

PhaseDuration::PhaseDuration() : _start{kNoDate}, _end{kNoDate} {}

boost::optional<Date_t> PhaseDuration::getStart() const {
    return readAtomicDate(_start);
}

boost::optional<Date_t> PhaseDuration::getEnd() const {
    return readAtomicDate(_end);
}

void PhaseDuration::setStart(Date_t date) {
    _start.store(date);
}

void PhaseDuration::setEnd(Date_t date) {
    _end.store(date);
}

boost::optional<Milliseconds> PhaseDuration::getElapsedMs(ClockSource* clock) const {
    auto start = getStart();
    if (!start) {
        return boost::none;
    }
    auto end = getEnd();
    return end.value_or_eval([=] { return clock->now(); }) - *start;
}

}  // namespace mongo
