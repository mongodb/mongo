// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class PhaseDuration {
public:
    PhaseDuration();
    boost::optional<Date_t> getStart() const;
    boost::optional<Date_t> getEnd() const;
    void setStart(Date_t date);
    void setEnd(Date_t date);

    template <typename TimeUnit>
    boost::optional<TimeUnit> getElapsed(ClockSource* clock) const {
        auto elapsed = getElapsedMs(clock);
        if (!elapsed) {
            return boost::none;
        }
        return duration_cast<TimeUnit>(*elapsed);
    }

private:
    boost::optional<Milliseconds> getElapsedMs(ClockSource* clock) const;

    Atomic<Date_t> _start;
    Atomic<Date_t> _end;
};

}  // namespace mongo
