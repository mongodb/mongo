// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/metrics/phase_duration.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
static constexpr auto kNoDate = Date_t::min();
boost::optional<Date_t> readAtomicDate(const Atomic<Date_t>& date) {
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
