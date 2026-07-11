// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <absl/hash/hash.h>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/key_extractors.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
// IWYU pragma: no_include "boost/multi_index/detail/adl_swap.hpp"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <limits>
#include <mutex>

#include <boost/multi_index/indexed_by.hpp>

namespace mongo::logv2 {

namespace bmi = boost::multi_index;

/**
 * Callable object yielding a variable LogSeverity depending on key and on timing.
 *
 * `bump(k)` returns `normal` severity on the
 * first call for a given `k`. This object will then internally mark `k` as being
 * suppressed for 1 sec, such that calls to `bump(k)` will return the less severe
 * `quiet` severity for key `k` until the suppression expires.
 */
template <typename K, typename H = absl::Hash<K>, typename Eq = std::equal_to<K>>
class [[MONGO_MOD_PUBLIC]] KeyedSeveritySuppressor {
public:
    using key_type = K;
    using hasher = H;
    using key_eq = Eq;

    /**
     * @param period The duration of the the suppression.
     * @param normal The debug level to use most of the time
     * @param quiet  The debug level to use at most once per period
     */
    KeyedSeveritySuppressor(Milliseconds period, LogSeverity normal, LogSeverity quiet)
        : _period{period}, _normal{normal}, _quiet{quiet} {}

private:
    struct Suppression {
        key_type key;
        Date_t expire;
    };

    /**
     * Indexed two ways:
     *     [0]: like a queue of Suppression with arrivals at the back.
     *     [1]: like a hashset of Suppression, keyed by their `key` member.
     */
    using Suppressions = bmi::multi_index_container<
        Suppression,
        bmi::indexed_by<bmi::sequenced<>,
                        bmi::hashed_unique<bmi::member<Suppression, key_type, &Suppression::key>,
                                           hasher,
                                           key_eq>>>;

public:
    /**
     * If there is an unexpired suppression on `k`, returns the quiet severity.
     * Otherwise, creates a fresh suppression on `k` and returns the normal severity.
     * Reaps expired suppressions.
     */
    LogSeverity operator()(const key_type& k) {
        auto now = Date_t::now();
        auto lg = std::lock_guard(_mutex);
        auto& queue = _suppressions.template get<0>();    // view as a queue
        auto& hashset = _suppressions.template get<1>();  // view as a hashset
        for (; !queue.empty() && queue.front().expire <= now;)
            queue.pop_front();
        if (!hashset.insert({k, {now + _period}}).second)
            return _quiet;  // There was an active suppression on `k`
        return _normal;
    }

private:
    Milliseconds _period;
    LogSeverity _normal;
    LogSeverity _quiet;
    std::mutex _mutex;
    Suppressions _suppressions;
};

class [[MONGO_MOD_PUBLIC]] SeveritySuppressor {
public:
    SeveritySuppressor(ClockSource* cs, Milliseconds period, LogSeverity normal, LogSeverity quiet)
        : _stopWatch(cs ? cs : SystemClockSource::get()),
          _period{durationCount<Milliseconds>(period)},
          _normal{normal},
          _quiet{quiet} {}

    SeveritySuppressor(Milliseconds period, LogSeverity normal, LogSeverity quiet)
        : SeveritySuppressor(nullptr, period, normal, quiet) {}

    LogSeverity operator()() {
        const auto elapsed = _stopWatch.elapsed();
        const Milliseconds period{_period.loadRelaxed()};
        auto periodStartedAtMillis{_periodStartedAtMillis.loadRelaxed()};
        if (Milliseconds{periodStartedAtMillis} + period <= elapsed) {
            const auto nextPeriodStartedAtMillis = durationCount<Milliseconds>(elapsed);
            if (_periodStartedAtMillis.compareAndSwap(&periodStartedAtMillis,
                                                      nextPeriodStartedAtMillis)) {
                return _normal;
            }
        }
        return _quiet;
    }

    void setPeriod(Milliseconds period) {
        _period.store(durationCount<Milliseconds>(period));
    }

    /**
     * Replaces the clock source and resets the period state so the next call returns the
     * normal (unsuppressed) severity regardless of elapsed time.
     * Not thread-safe, so use only for testing purposes.
     */
    [[MONGO_MOD_PUBLIC]] void resetWithClockSource_forTest(ClockSource* cs) {
        _stopWatch = ClockSource::StopWatch{cs ? cs : SystemClockSource::get()};
        _periodStartedAtMillis.store(std::numeric_limits<int64_t>::min());
    }

private:
    ClockSource::StopWatch _stopWatch;
    Atomic<int64_t> _period;
    const LogSeverity _normal;
    const LogSeverity _quiet;
    Atomic<int64_t> _periodStartedAtMillis{
        std::numeric_limits<int64_t>::min()};  // Ensure the first severity is not suppressed
};

}  // namespace mongo::logv2
