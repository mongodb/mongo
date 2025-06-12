/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/metrics/phase_duration.h"
#include "mongo/s/resharding/common_types_gen.h"

namespace mongo {

template <typename PhaseEnum, size_t Size>
class PhaseDurationTracker {
public:
    using TimedPhaseNameMap = stdx::unordered_map<PhaseEnum, StringData>;

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

    static boost::optional<StringData> getNameFor(PhaseEnum phase, const TimedPhaseNameMap& names) {
        auto it = names.find(phase);
        if (it == names.end()) {
            return boost::none;
        }
        return it->second;
    }

    std::array<PhaseDuration, Size> _durations;
};

}  // namespace mongo
