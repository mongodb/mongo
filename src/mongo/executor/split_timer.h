/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <array>
#include <boost/optional.hpp>
#include <fmt/format.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {

namespace split_timer_detail {

/**
 * Takes an invokable `f` and a pack of optionals `as...`.
 * Invokes `f(*as...)`, returning the result as an optional.
 * If any of `as...` are disengaged, then `f` is not
 * invoked, and a disengaged result is returned.
 */
template <typename F,
          typename... As,
          typename R = std::invoke_result_t<F, decltype(*std::declval<As>())...>>
boost::optional<R> applyViaOptionals(F&& f, As&&... as) {
    if (!(as && ...))
        return {};
    return std::invoke(f, *as...);
}

/** Rule of Zero utility. Starts true, becomes false when moved from or dismissed. */
class UniqueActive {
public:
    UniqueActive() = default;

    UniqueActive(UniqueActive&& other) noexcept : _active{other.dismiss()} {}

    UniqueActive& operator=(UniqueActive&& other) noexcept {
        if (this != &other)
            _active = other.dismiss();
        return *this;
    }

    explicit operator bool() const {
        return _active;
    }

    /** Returns true if dismiss changed this object's state. */
    bool dismiss() {
        return std::exchange(_active, false);
    }

private:
    bool _active = true;
};
}  // namespace split_timer_detail

/**
 * Acts as a split timer which captures times elapsed at various points throughout a single
 * SessionWorkflow loop. The SessionWorkflow loop is expected to construct this
 * object when timing should begin, call this object's `notify` function at
 * appropriate times throughout the workflow, and destroy it when the loop work is done.
 *
 * Requirements on Policy:
 *  - TimeSplitIdType and IntervalIdType packed enums.
 *  - static constexpr properties:
 *      - size_t numIntervalIds and numTimesplitIds for enum extents.
 *      - toIdx(e) to convert these enums to size_t.
 *      - getName(e) to convert these enums to StringData (intervals should end in "Millis").
 *      - TimeSplitIdType getStartSplit(IntervalIdType).
 *      - TimeSplitIdType getEndSplit(IntervalIdType).
 *  - nonstatic onStart and onFinish member functions, taking a pointer to this SplitTimer.
 *  - nonstatic makeTimer to initialize Timer.
 */
template <typename Policy>
class SplitTimer {
private:
    template <typename E>
    static constexpr size_t _idx(E e) {
        return Policy::toIdx(e);
    }

public:
    using TimeSplitIdType = typename Policy::TimeSplitIdType;
    using IntervalIdType = typename Policy::IntervalIdType;

    SplitTimer() : SplitTimer{Policy{}} {}
    SplitTimer(Policy policy) : _policy{std::move(policy)} {
        _policy.onStart(this);
    }

    SplitTimer& operator=(SplitTimer&&) = default;
    SplitTimer(SplitTimer&&) = default;

    /**
     * Idempotent: if already inactive (moved-from), does nothing.
     * If active: sets to inactive, captures the elapsed time for the `done`
     * TimeSplitId, logs any necessary metrics.
     */
    ~SplitTimer() {
        if (!_active)
            return;
        _policy.onFinish(this);
    }

    /**
     * Captures the elapsed time and associates it with `split`. A second call with the same `split`
     * will overwrite the previous. It is expected that this gets called for all splits other than
     * TimerSplit::start and TimerSplit::done.
     */
    void notify(TimeSplitIdType split) {
        using namespace fmt::literals;
        invariant(!_topSplit || _idx(*_topSplit) <= _idx(split),
                  "Notify out of order: {} then {}"_format(_policy.getName(*_topSplit),
                                                           _policy.getName(split)));
        _topSplit = split;

        _splits[_idx(split)] = _timer.elapsed();
    }

    /**
     * Returns the time elapsed between the two splits corresponding to `start` and `end`.
     * If either is disengaged, a disengaged optional will be returned.
     */
    boost::optional<Microseconds> getSplitInterval(IntervalIdType id) const {
        return split_timer_detail::applyViaOptionals(std::minus<>{},
                                                     _splits[_idx(_policy.getEndSplit(id))],
                                                     _splits[_idx(_policy.getStartSplit(id))]);
    }

    /**
     * Appends the intervals defined in Policy::intervalDefs to `builder`.
     * Logs the whole builder if there's a problem.
     */
    void appendIntervals(BSONObjBuilder& builder) const {
        using namespace fmt::literals;
        for (size_t i = 0; i != Policy::numIntervalIds; ++i) {
            IntervalIdType iId{i};
            auto dt = getSplitInterval(iId);
            if (!dt)
                continue;
            if (*dt < Microseconds{0})
                LOGV2_FATAL(6983001,
                            "Negative time interval",
                            "dt"_attr = dt->toString(),
                            "splits"_attr = _splitsToBSON());
            builder.append(_policy.getName(iId), durationCount<Milliseconds>(*dt));
        }
    }

private:
    using SplitsArray = std::array<boost::optional<Microseconds>, Policy::numTimeSplitIds>;

    BSONObj _splitsToBSON() const {
        BSONObjBuilder bob;
        for (size_t i = 0; i != Policy::numTimeSplitIds; ++i) {
            TimeSplitIdType ts{i};
            auto&& t = _splits[_idx(ts)];
            bob.append(_policy.getName(ts), t ? t->toString() : "");
        }
        return bob.obj();
    }

    Policy _policy;
    split_timer_detail::UniqueActive _active;
    Timer _timer = _policy.makeTimer();
    SplitsArray _splits;
    boost::optional<TimeSplitIdType> _topSplit;
};
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
