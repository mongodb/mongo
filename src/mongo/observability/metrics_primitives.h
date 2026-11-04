// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/aligned.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo::observability {

/// The first double past the int64 range (2^63): every int64_t is strictly below it, so a
/// boundary at or above it exceeds any signed observation without a cast.
inline constexpr double kFirstDoubleAboveInt64 = 0x1p63;

/// The first double past the uint64 range (2^64): every uint64_t is strictly below it, so a
/// boundary at or above it exceeds any unsigned observation without a cast. Also the point
/// beyond which a floating-point sum contribution saturates at UINT64_MAX.
inline constexpr double kFirstDoubleAboveUint64 = 0x1p64;

/**
 * The storage and recording path shared by Counter and UpDownCounter.
 */
template <typename T>
class [[MONGO_MOD_PUBLIC]] AdditiveCounter {
public:
    using value_type = T;

    T operator()() const {
        return _value.loadRelaxed();
    }

protected:
    AdditiveCounter() = default;

    void addUnchecked(T value) {
        _value.fetchAndAddRelaxed(value);
    }

private:
    Atomic<T> _value{0};
};

/**
 * A monotonically increasing counter.
 *
 * Increments must be nonnegative.
 */
template <typename T>
class [[MONGO_MOD_PUBLIC]] Counter : public AdditiveCounter<T> {
public:
    using value_type = T;

    Counter() = default;

    void add(T value) {
        tassert(ErrorCodes::BadValue, "Counter increment must be nonnegative", value >= 0);
        AdditiveCounter<T>::addUnchecked(value);
    }
};

/**
 * A counter that may increase or decrease.
 */
template <typename T>
class [[MONGO_MOD_PUBLIC]] UpDownCounter : public AdditiveCounter<T> {
public:
    using value_type = T;

    UpDownCounter() = default;

    void add(T value) {
        AdditiveCounter<T>::addUnchecked(value);
    }
};

/**
 * A point-in-time value.
 */
template <typename T>
class [[MONGO_MOD_PUBLIC]] Gauge {
public:
    using value_type = T;

    void set(T value) {
        _value.storeRelaxed(value);
    }

    T operator()() const {
        return _value.loadRelaxed();
    }

private:
    Atomic<T> _value{0};
};

namespace detail {

/**
 * Spins rather than blocking; the windowed critical sections are a handful of relaxed stores.
 * Yields while contended so a spinning thread does not starve a descheduled holder.
 */
class WindowLock {
public:
    void lock() {
        while (_flag.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void unlock() {
        _flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag _flag{};
};

}  // namespace detail

/**
 * A high-water-mark accumulator. Read the current maximum with operator().
 *
 * The mark starts at zero and is intended for non-negative quantities such as sizes, counts, and
 * utilizations. A setToMax() of a negative value is ignored, since no negative value exceeds the
 * zero baseline. A quantity whose maximum can be negative should be seeded with reset() to
 * establish its starting baseline.
 *
 * reset(value) establishes a new baseline by storing `value` unconditionally. setToMax() never
 * lowers the value. operator() is read-only.
 */
template <typename T>
class [[MONGO_MOD_PUBLIC]] HighWaterMark {
public:
    using value_type = T;

    HighWaterMark() = default;

    /** Raises the stored value to `value`; a smaller `value` is ignored. */
    void setToMax(T value) {
        T cur = _value.loadRelaxed();
        while (value > cur) {
            if (_value.compareAndSwapWeak(&cur, value)) {
                return;
            }
        }
    }

    /** Lowers the stored value to `value` unconditionally, establishing a new baseline. */
    void reset(T value) {
        _value.storeRelaxed(value);
    }

    T operator()() const {
        return _value.loadRelaxed();
    }

private:
    Atomic<T> _value{0};
};

/**
 * A high-water-mark accumulator that restarts from the incoming value once a time window expires.
 * Read the current maximum with operator().
 *
 * There is no background thread: each setToMax() checks how much time has elapsed since the
 * current window started, and if at least the window has passed it restarts the window with the
 * incoming value as the new baseline. reset(value) also restarts the window.
 *
 * resetAfter(window) sets the window length, discarding any previous window: the next recording
 * call starts a fresh one from its own value. Passing a zero duration disables time-based reset,
 * leaving a plain cumulative maximum.
 *
 * operator() is read-only; it reports the stale maximum until the next recording call rolls the
 * window.
 *
 * Recording is serialized by a spin lock, since a rollover lowers the value unconditionally and
 * would otherwise erase a concurrent in-window update. Reconfiguring the window concurrently with
 * recording remains best-effort: two callers that both observe an expired window may each restart
 * it.
 *
 * Time is read through the Clock template parameter, which defaults to std::chrono::steady_clock.
 */
template <typename T, typename Clock = std::chrono::steady_clock>
class [[MONGO_MOD_PUBLIC]] TimedHighWaterMark {
public:
    using value_type = T;

    TimedHighWaterMark() = default;

    /** Sets the window length and discards any previous window. */
    void resetAfter(std::chrono::steady_clock::duration window) {
        _windowNanos.storeRelaxed(
            std::chrono::duration_cast<std::chrono::nanoseconds>(window).count());
        _windowBegun.storeRelaxed(false);
    }

    /** Raises the stored value to `value`; a smaller `value` restarts an expired window instead. */
    void setToMax(T value) {
        std::lock_guard<detail::WindowLock> lock(_windowLock);
        if (MONGO_unlikely(_windowNanos.loadRelaxed() != 0)) {
            _rollWindowIfExpired(value);
        }
        _watermark.setToMax(value);
    }

    /**
     * Lowers the stored value to `value` unconditionally, establishing a new baseline and
     * restarting the time window.
     */
    void reset(T value) {
        std::lock_guard<detail::WindowLock> lock(_windowLock);
        _watermark.reset(value);
        if (MONGO_unlikely(_windowNanos.loadRelaxed() != 0)) {
            _windowStartNanos.storeRelaxed(_nowNanos());
            _windowBegun.storeRelaxed(true);
        }
    }

    T operator()() const {
        return _watermark();
    }

private:
    /**
     * Restarts the time window if it has expired, seeding the new window with `value`. Called only
     * from setToMax(), with the window lock held.
     */
    void _rollWindowIfExpired(T value) {
        const auto windowNanos = _windowNanos.loadRelaxed();
        const auto nowNanos = _nowNanos();
        if (_windowBegun.loadRelaxed() &&
            nowNanos - _windowStartNanos.loadRelaxed() < windowNanos) {
            return;
        }
        _windowStartNanos.storeRelaxed(nowNanos);
        _windowBegun.storeRelaxed(true);
        _watermark.reset(value);
    }

    static std::int64_t _nowNanos() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count();
    }

    HighWaterMark<T> _watermark;
    // Nanoseconds per window; zero disables time-based reset.
    Atomic<std::int64_t> _windowNanos{0};
    // Start of the current window in steady_clock nanoseconds; whether one has begun is tracked
    // by _windowBegun.
    Atomic<std::int64_t> _windowStartNanos{0};
    Atomic<bool> _windowBegun{false};
    detail::WindowLock _windowLock;
};

/**
 * The bucket boundaries used for histograms.
 *
 * Boundaries are inclusive lower bounds: with n boundaries there are n + 1 buckets, the first
 * covering (-inf, boundaries[0]), bucket i covering [boundaries[i-1], boundaries[i]), and the last
 * covering [boundaries[n-1], +inf). A value equal to a boundary therefore lands in the upper
 * bucket, and a value above the last boundary saturates into the top bucket.
 *
 * Boundaries must be sorted and must not contain NaN.
 */
template <typename T>
class BucketBoundaries {
public:
    using value_type = T;

    explicit BucketBoundaries(std::vector<double> boundaries = {})
        : _boundaries(std::move(boundaries)) {
        tassert(ErrorCodes::BadValue,
                "Histogram boundaries must not contain NaN",
                std::none_of(_boundaries.begin(), _boundaries.end(), [](double boundary) {
                    return std::isnan(boundary);
                }));
        tassert(ErrorCodes::BadValue,
                "Histogram boundaries must be sorted",
                std::is_sorted(_boundaries.begin(), _boundaries.end()));
    }

    /** The first boundary strictly greater than `value`; a value on a boundary moves up. */
    std::size_t bucketIndex(T value) const {
        const auto firstGreater = std::upper_bound(
            _boundaries.begin(), _boundaries.end(), value, [](T key, double boundary) {
                return BucketBoundaries<T>::_boundaryExceedsValue(boundary, key);
            });
        return static_cast<std::size_t>(firstGreater - _boundaries.begin());
    }

    /** One more than the number of boundaries: the count of buckets. */
    std::size_t bucketCount() const {
        return _boundaries.size() + 1;
    }

    const std::vector<double>& boundaries() const {
        return _boundaries;
    }

private:
    /**
     * Compares `boundary > value` exactly.
     *
     * Floating-point observations compare directly. An integral observation must not convert to
     * double (doubles cannot represent integers above 2^53, so INT64_MAX would round to 2^63 and
     * misbucket against a boundary sitting exactly there).
     */
    static bool _boundaryExceedsValue(double boundary, T value) {
        if constexpr (std::is_floating_point_v<T>) {
            return boundary > value;
        } else {
            const double floorBoundary = std::floor(boundary);
            return _integerValuedDoubleComparedTo(
                floorBoundary, value, /*boundaryIsIntegral=*/floorBoundary == boundary);
        }
    }

    /**
     * Compares an integer-valued double `boundary` against integral `value` exactly, as
     * `boundary > value` when `boundaryIsIntegral` and `boundary >= value` otherwise.
     */
    static bool _integerValuedDoubleComparedTo(double boundary, T value, bool boundaryIsIntegral) {
        if constexpr (std::is_signed_v<T>) {
            if (boundary >= kFirstDoubleAboveInt64) {
                return true;  // Exceeds every signed 64-bit value.
            }
            if (boundary < -kFirstDoubleAboveInt64) {  // Below every signed 64-bit value.
                return false;
            }
            const auto boundaryInt = static_cast<std::int64_t>(boundary);
            const auto valueInt = static_cast<std::int64_t>(value);
            return boundaryIsIntegral ? boundaryInt > valueInt : boundaryInt >= valueInt;
        } else {
            if (boundary >= kFirstDoubleAboveUint64) {
                return true;  // Exceeds every unsigned 64-bit value.
            }
            if (boundary < 0.0) {
                return false;  // Below every unsigned value.
            }
            const auto boundaryInt = static_cast<std::uint64_t>(boundary);
            const auto valueInt = static_cast<std::uint64_t>(value);
            return boundaryIsIntegral ? boundaryInt > valueInt : boundaryInt >= valueInt;
        }
    }

    std::vector<double> _boundaries;
};

/**
 * A lock-free histogram that counts observations into buckets delimited by explicit boundaries.
 *
 * observe() performs three relaxed fetch-and-adds (count, sum, bucket) and takes no lock.
 *
 * The following describe characteristics of observation by type:
 *  - The sum is an unsigned integer.
 *  - A floating-point observation is truncated toward zero.
 *  - A negative floating-point observation still increments the count and its bucket but
 * contributes 0 to the sum, since a negative value has no representation in the unsigned sum.
 *  - A floating-point observation above the uint64 range, including +infinity, contributes
 * UINT64_MAX to the sum. The accumulated sum itself wraps modulo 2^64.
 *  - An integral observation is cast to the unsigned sum type by the usual modular conversion, so a
 * negative value wraps (for example, an int64_t observation of -1 adds 2^64 - 1).
 *
 * min and max are not tracked. Use Histogram instead when exact min/max or a signed floating-point
 * sum is required.
 */
template <typename T>
class [[MONGO_MOD_PUBLIC]] AtomicHistogram {
public:
    using value_type = T;

    struct Snapshot {
        std::uint64_t count{0};
        std::uint64_t sum{0};
        // buckets.size() == boundaries.size() + 1.
        std::vector<std::uint64_t> buckets;
    };

    explicit AtomicHistogram(std::vector<double> boundaries = {})
        : _boundaries(std::move(boundaries)), _buckets(_boundaries.bucketCount()) {}

    /** Records one observation. */
    void observe(T value) {
        const auto index = _boundaries.bucketIndex(value);
        _count.fetchAndAddRelaxed(1);
        _sum.fetchAndAddRelaxed(_sumContribution(value));
        _buckets[index].fetchAndAddRelaxed(1);
    }

    /**
     * Returns a snapshot built from relaxed loads. A snapshot taken while other threads are
     * observing may be torn since each field is an independent relaxed load.
     */
    Snapshot snapshot() const {
        Snapshot snapshot;
        snapshot.count = _count.loadRelaxed();
        snapshot.sum = _sum.loadRelaxed();
        snapshot.buckets.reserve(_buckets.size());
        for (const auto& bucket : _buckets) {
            snapshot.buckets.push_back(bucket.loadRelaxed());
        }
        return snapshot;
    }

    /** The inclusive lower bounds of the buckets, one more bucket than boundaries. */
    const std::vector<double>& boundaries() const {
        return _boundaries.boundaries();
    }

private:
    /**
     * The unsigned contribution of one observation to the sum.
     *
     * A floating-point observation at or above 2^64, including +infinity, saturates at UINT64_MAX
     * rather than invoking undefined behaviour in the cast.
     */
    static std::uint64_t _sumContribution(T value) {
        if constexpr (std::is_floating_point_v<T>) {
            if (value >= kFirstDoubleAboveUint64) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return value > 0 ? static_cast<std::uint64_t>(value) : 0;
        } else {
            return static_cast<std::uint64_t>(value);
        }
    }

    BucketBoundaries<T> _boundaries;
    Atomic<std::uint64_t> _count{0};
    Atomic<std::uint64_t> _sum{0};
    std::vector<Atomic<std::uint64_t>> _buckets;
};

/**
 * A mutex-guarded histogram that tracks a count, sum, min, and max alongside its buckets.
 *
 * observe() takes a lock, so the recorded statistics are exact except for the sum: the count and
 * the buckets are exact, and min and max keep the observation type T so integral extrema above 2^53
 * do not round. The sum accumulates in double precision and loses exactness for integral
 * observations above 2^53.
 *
 * min and max are unset until the first observation. An observation of NaN makes min and max
 * meaningless, just as it does the sum.
 *
 * Prefer AtomicHistogram when the recording path is hot and approximate totals are acceptable.
 */
template <typename T>
class [[MONGO_MOD_PUBLIC]] Histogram {
public:
    using value_type = T;

    struct Snapshot {
        std::uint64_t count{0};
        double sum{0};
        // Unset until the first observation.
        std::optional<T> min{};
        std::optional<T> max{};
        // buckets.size() == boundaries.size() + 1.
        std::vector<std::uint64_t> buckets;
    };

    explicit Histogram(std::vector<double> boundaries = {})
        : _boundaries(std::move(boundaries)),
          _snapshot{.buckets = std::vector<std::uint64_t>(_boundaries.bucketCount())} {}

    /** Records one observation. */
    void observe(T value) {
        std::lock_guard lock(_mutex);
        ++_snapshot.count;
        _snapshot.sum += static_cast<double>(value);
        if (!_snapshot.min.has_value() || value < *_snapshot.min) {
            _snapshot.min = value;
        }
        if (!_snapshot.max.has_value() || value > *_snapshot.max) {
            _snapshot.max = value;
        }
        ++_snapshot.buckets[_boundaries.bucketIndex(value)];
    }

    /** Returns an exact copy of the recorded state, taken under the lock. */
    Snapshot snapshot() const {
        std::lock_guard lock(_mutex);
        return _snapshot;
    }

    /** The inclusive lower bounds of the buckets, one more bucket than boundaries. */
    const std::vector<double>& boundaries() const {
        return _boundaries.boundaries();
    }

private:
    BucketBoundaries<T> _boundaries;
    mutable std::mutex _mutex;
    Snapshot _snapshot;
};

/**
 * A metric primitive for a non-scalar value that cannot live in an atomic, such as a std::string or
 * a BSONObj.
 *
 * A mutex serializes access, so the primitive is safe to update and read concurrently.
 *
 * Reserve this for non-scalar leaves that are written rarely and read only for rendering. A
 * hot-path leaf whose value is a scalar belongs in one of the atomic primitives above instead.
 */
template <typename T>
class [[MONGO_MOD_PUBLIC]] SynchronizedValue {
public:
    using value_type = T;

    /** Stores a copy of `value`, replacing any previous value. */
    void set(T value) {
        std::lock_guard lock(_mutex);
        _value = std::move(value);
    }

    /** Returns a copy of the stored value. */
    T get() const {
        std::lock_guard lock(_mutex);
        return _value;
    }

    /** Aliases get(). */
    T operator()() const {
        return get();
    }

private:
    mutable std::mutex _mutex;
    T _value{};
};

/**
 * A metric primitive that may or may not be ever recorded to. OptionalMetric wraps any of the
 * primitives above and tracks its own written flag.
 *
 * Every recording call marks the wrapper written, even one that records a zero or otherwise leaves
 * the value unchanged.
 *
 * Helpers are written such that isWritten() returning true means that the recording call has been
 * completed and effects are visible to the caller.
 */
template <typename Primitive>
class [[MONGO_MOD_PUBLIC]] OptionalMetric {
public:
    OptionalMetric() = default;

    template <typename... Args>
    explicit OptionalMetric(Args&&... args) : _primitive(std::forward<Args>(args)...) {}

    void add(auto value) {
        _primitive.add(value);
        _markWritten();
    }

    void set(auto value) {
        _primitive.set(value);
        _markWritten();
    }

    void observe(auto value) {
        _primitive.observe(value);
        _markWritten();
    }

    void setToMax(auto value) {
        _primitive.setToMax(value);
        _markWritten();
    }

    void reset(auto value) {
        _primitive.reset(value);
        _markWritten();
    }

    void resetAfter(std::chrono::steady_clock::duration window) {
        _primitive.resetAfter(window);
    }

    auto operator()() const {
        return _primitive();
    }

    auto snapshot() const {
        return _primitive.snapshot();
    }

    /**
     * True once any recording call has occurred, even one that recorded a zero.
     */
    bool isWritten() const {
        return _written.load();
    }

    /**
     * Returns the wrapped primitive for read-only inspection.
     */
    const Primitive& get() const {
        return _primitive;
    }

private:
    void _markWritten() {
        _written.store(true);
    }

    Primitive _primitive{};
    // The wrapper owns its written flag rather than delegating to the primitive, which need not
    // have one at all.
    Atomic<bool> _written{false};
};

}  // namespace mongo::observability
