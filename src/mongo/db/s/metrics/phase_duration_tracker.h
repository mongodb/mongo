// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/metrics/phase_duration.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

template <typename PhaseEnum, size_t Size>
class PhaseDurationTracker {
public:
    using TimedPhaseNameMap = stdx::unordered_map<PhaseEnum, std::string_view>;

    boost::optional<ReshardingMetricsTimeInterval> getIntervalFor(PhaseEnum phase) const {
        auto start = getStartFor(phase);
        auto end = getEndFor(phase);
        if (!start && !end) {
            return boost::none;
        }
        ReshardingMetricsTimeInterval interval;
        interval.setStart(start);
        interval.setStop(end);
        return interval;
    }

    boost::optional<Date_t> getStartFor(PhaseEnum phase) const {
        return _durations[toIndex(phase)].getStart();
    }

    boost::optional<Date_t> getEndFor(PhaseEnum phase) const {
        return _durations[toIndex(phase)].getEnd();
    }

    void setStartFor(PhaseEnum phase, Date_t date) {
        if (!_durations[toIndex(phase)].getStart()) {
            return _durations[toIndex(phase)].setStart(date);
        }
    }

    void setEndFor(PhaseEnum phase, Date_t date) {
        if (!_durations[toIndex(phase)].getEnd()) {
            return _durations[toIndex(phase)].setEnd(date);
        }
    }

    template <typename TimeUnit>
    boost::optional<TimeUnit> getElapsed(PhaseEnum phase, ClockSource* clock) const {
        return _durations[toIndex(phase)].template getElapsed<TimeUnit>(clock);
    }

    /**
     * Returns the elapsed time between the start of 'startPhase' and the end of 'endPhase'.
     * Returns boost::none if either timestamp is absent or if end precedes start.
     */
    template <typename TimeUnit>
    boost::optional<TimeUnit> getCrossPhaseElapsed(PhaseEnum startPhase, PhaseEnum endPhase) const {
        auto start = getStartFor(startPhase);
        auto end = getEndFor(endPhase);
        if (!start || !end || *end < *start) {
            return boost::none;
        }
        return duration_cast<TimeUnit>(*end - *start);
    }

    template <typename TimeUnit>
    void reportDurationsForAllPhases(const TimedPhaseNameMap& names,
                                     ClockSource* clock,
                                     BSONObjBuilder* bob,
                                     boost::optional<TimeUnit> defaultValue = boost::none) const {
        using IntType = std::underlying_type_t<PhaseEnum>;
        for (IntType i = 0; i < static_cast<IntType>(Size); i++) {
            PhaseEnum phase{static_cast<PhaseEnum>(i)};
            auto name = getNameFor(phase, names);
            auto duration = getElapsed<TimeUnit>(phase, clock);
            duration = duration ? duration : defaultValue;
            if (!name || !duration) {
                continue;
            }
            bob->append(*name, (*duration).count());
        }
    }

private:
    size_t toIndex(PhaseEnum phase) const {
        auto index = static_cast<size_t>(phase);
        invariant(index >= 0);
        invariant(index < Size);
        return index;
    }

    static boost::optional<std::string_view> getNameFor(PhaseEnum phase,
                                                        const TimedPhaseNameMap& names) {
        auto it = names.find(phase);
        if (it == names.end()) {
            return boost::none;
        }
        return it->second;
    }

    std::array<PhaseDuration, Size> _durations;
};

}  // namespace mongo
