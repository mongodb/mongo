// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

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
 *      - getName(e) to convert these enums to std::string_view (intervals should end in "Millis").
 *      - TimeSplitIdType getStartSplit(IntervalIdType).
 *      - TimeSplitIdType getEndSplit(IntervalIdType).
 *  - nonstatic onStart and onFinish member functions, taking a pointer to this SplitTimer.
 *  - nonstatic makeTimer to initialize Timer.
 */
template <typename Policy>
class [[MONGO_MOD_PUBLIC]] SplitTimer {
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
        invariant(!_topSplit || _idx(*_topSplit) <= _idx(split),
                  fmt::format("Notify out of order: {} then {}",
                              _policy.getName(*_topSplit),
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
