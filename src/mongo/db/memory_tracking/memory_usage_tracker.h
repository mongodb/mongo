/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include <boost/noncopyable.hpp>

namespace mongo {
/**
 * Memory usage tracker for use cases where we don't need per-function memory tracking.
 *
 * TODO SERVER-113197: Remove streams dependency on this class.
 */
class MONGO_MOD_NEEDS_REPLACEMENT SimpleMemoryUsageTracker {
public:
    SimpleMemoryUsageTracker(const SimpleMemoryUsageTracker&) = delete;
    SimpleMemoryUsageTracker operator=(const SimpleMemoryUsageTracker&) = delete;

    SimpleMemoryUsageTracker(SimpleMemoryUsageTracker&&) = default;
    SimpleMemoryUsageTracker& operator=(SimpleMemoryUsageTracker&&) = default;

    SimpleMemoryUsageTracker(SimpleMemoryUsageTracker* base,
                             int64_t maxAllowedMemoryUsageBytes,
                             int64_t chunkSize = 0);

    explicit SimpleMemoryUsageTracker(int64_t maxAllowedMemoryUsageBytes, int64_t chunkSize = 0);

    SimpleMemoryUsageTracker();

    void add(int64_t diff);
    void set(int64_t total);

    int64_t inUseTrackedMemoryBytes() const {
        return _inUseTrackedMemoryBytes;
    }

    int64_t peakTrackedMemoryBytes() const {
        return _peakTrackedMemoryBytes;
    }

    bool withinMemoryLimit() const {
        return _inUseTrackedMemoryBytes <= _maxAllowedMemoryUsageBytes;
    }

    int64_t maxAllowedMemoryUsageBytes() const {
        return _maxAllowedMemoryUsageBytes;
    }

    /**
     * Returns a new SimpleMemoryUsageTracker. The copy constructor for this class is purposefully
     * deleted - use this method instead. Note that the members _peakTrackedMemoryBytes and
     * _inUseTrackedMemoryBytes will be initialized to zero.
     */
    SimpleMemoryUsageTracker makeFreshSimpleMemoryUsageTracker() const;

    friend class MemoryUsageTracker;

protected:
    /**
     * Provide an extra function that is called whenever add() is invoked. Let it be set via this
     * method instead in the constructor to allow subclasses to capture "this."
     */
    void setWriteToCurOp(std::function<void(int64_t, int64_t)> writeToCurOp);

private:
    SimpleMemoryUsageTracker* _base = nullptr;

    // Maximum memory consumption thus far observed for this function.
    int64_t _peakTrackedMemoryBytes = 0;
    // Tracks the current memory footprint.
    int64_t _inUseTrackedMemoryBytes = 0;

    int64_t _maxAllowedMemoryUsageBytes;

    // Allow for some extra bookkeeping to be done when add() is called. If set, this function will
    // be invoked with _inUseTrackedMemoryBytes and _peakTrackedMemoryBytes. This mechanism exists
    // to avoid making add() virtual, since it has been shown to have an effect on performance in
    // some cases.
    std::function<void(int64_t, int64_t)> _writeToCurOp;

    // If set, memory usage updates will only be written to CurOp if the usage surpasses this
    // size. Writing to CurOp involves lock contention, so in performance-sensitive situations,
    // we should set a non-zero size. If 0, no chunking is performed.
    int64_t _chunkSize;
    // Last lower-bound chunk reported to CurOp.
    int64_t _lastReportedLowerBound = 0;
};

/**
 * This is a utility class for tracking memory usage across multiple arbitrary operators or
 * functions, which are identified by their string names. Tracks both current and highest
 * encountered memory consumption.
 *
 * It can be used directly by calling MemoryUsageTracker::add(int64_t diff), or by creating a
 * dependent tracker via MemoryUsageTracker::operator[].
 *
 * Dependent tracker will update both its own memory and the total. It is used to track the
 * consumption of individual parts, such as different accumulators in $group, while simultaneously
 * keeping track of the total.
 *
 * Cannot be shallow copied because child memory trackers point to the address of the inline
 * base tracker of the class.
 *
 * TODO SERVER-113197: Remove streams dependency on this class.
 */
class MONGO_MOD_NEEDS_REPLACEMENT MemoryUsageTracker {
public:
    MemoryUsageTracker(const MemoryUsageTracker&) = delete;
    MemoryUsageTracker operator=(const MemoryUsageTracker&) = delete;

    MemoryUsageTracker(MemoryUsageTracker&&) = default;
    MemoryUsageTracker& operator=(MemoryUsageTracker&&) = default;

    MemoryUsageTracker(SimpleMemoryUsageTracker* baseParent,
                       bool allowDiskUse = false,
                       int64_t maxMemoryUsageBytes = 0,
                       int64_t chunkSize = 0);

    MemoryUsageTracker(bool allowDiskUse = false, int64_t maxMemoryUsageBytes = 0);

    /**
     * Sets the new total for 'name', and updates the current total memory usage.
     */
    void set(StringData name, int64_t total);

    /**
     * Resets both the total memory usage as well as the per-function memory usage, but retains the
     * current value for maximum total memory usage.
     */
    void resetCurrent();

    /**
     * Clears the child memory trackers map and resets the base tracker memory usage to zero.
     */
    void clear();

    /**
     * Non-const version, creates a new element if one doesn't exist and returns a reference to it.
     */
    SimpleMemoryUsageTracker& operator[](StringData name);

    /**
     * Updates the memory usage for 'name' by adding 'diff' to the current memory usage for
     * that function. Also updates the total memory usage.
     */
    void add(StringData name, int64_t diff);

    /**
     * Updates total memory usage.
     */
    void add(int64_t diff) {
        _baseTracker.add(diff);
    }

    auto inUseTrackedMemoryBytes() const {
        return _baseTracker.inUseTrackedMemoryBytes();
    }
    auto peakTrackedMemoryBytes() const {
        return _baseTracker.peakTrackedMemoryBytes();
    }

    int64_t peakTrackedMemoryBytes(StringData name) const;

    bool withinMemoryLimit() const {
        return _baseTracker.withinMemoryLimit();
    }

    bool allowDiskUse() const {
        return _allowDiskUse;
    }

    int64_t maxAllowedMemoryUsageBytes() const {
        return _baseTracker.maxAllowedMemoryUsageBytes();
    }

    /**
     * Returns a new MemoryUsageTracker. The copy constructor for this class is purposefully
     * deleted - use this method instead. Note that the function memory tracker table will be
     * initialized as empty.
     */
    MemoryUsageTracker makeFreshMemoryUsageTracker() const;

private:
    bool _allowDiskUse;
    // Tracks current memory used. This tracker rolls up memory usage from all trackers in the
    // function memory tracker table.
    SimpleMemoryUsageTracker _baseTracker;
    // Tracks memory consumption per function using the output field name as a key.
    stdx::unordered_map<std::string, SimpleMemoryUsageTracker> _functionMemoryTracker;
};

/**
 * This is a utility class for tracking memory usage for record deduplication across multiple
 * arbitrary operators. Optionally, it can be used to report the metrics to the serverStatus
 * command.
 */
class MONGO_MOD_NEEDS_REPLACEMENT DeduplicatorReporter {
public:
    DeduplicatorReporter(const DeduplicatorReporter&) = delete;
    DeduplicatorReporter& operator=(const DeduplicatorReporter&) = delete;

    DeduplicatorReporter(DeduplicatorReporter&&) noexcept = default;
    DeduplicatorReporter& operator=(DeduplicatorReporter&&) noexcept = default;

    DeduplicatorReporter(std::function<void(int64_t, int64_t)> callback, int64_t chunkSize);

    void add(int64_t diff);

private:
    // Tracks the current memory footprint.
    int64_t _inUseTrackedMemoryBytes = 0;
    int64_t _inUseRecordIdCount = 0;

    // Allow for some extra bookkeeping to be done when add() is called. If set, this function will
    // be invoked with _inUseTrackedMemoryBytes and _inUseRecordIdCount.
    std::function<void(int64_t, int64_t)> _reportCallback;

    // If set, memory usage updates will only be written to serverStatus if the usage surpasses this
    // size. Writing to serverStatus involves lock contention, so in performance-sensitive
    // situations, we should set a non-one size. If 1, no chunking is performed.
    int64_t _chunkSize;
    // Last lower-bound chunk reported to serverStatus. Same as _inUseTrackedMemoryBytes and
    // _inUseRecordIdCount if _chunkSize is 1 (no chunking).
    int64_t _lastReportedLowerBound = 0;
    int64_t _lastReportedRecordIdCount = 0;
};

/**
 * TODO SERVER-113197: Remove streams dependency on this class.
 */
template <typename Tracker>
class MONGO_MOD_OPEN MemoryUsageTokenImpl : private boost::noncopyable {
public:
    // Default constructor is only present to support ease of use for some containers.
    MemoryUsageTokenImpl() {}

    MemoryUsageTokenImpl(size_t initial, Tracker* tracker)
        : _tracker(tracker), _curMemoryUsageBytes(initial) {
        _tracker->add(_curMemoryUsageBytes);
    }

    MemoryUsageTokenImpl(MemoryUsageTokenImpl&& other)
        : _tracker(other._tracker), _curMemoryUsageBytes(other._curMemoryUsageBytes) {
        other._tracker = nullptr;
    }

    MemoryUsageTokenImpl& operator=(MemoryUsageTokenImpl&& other) {
        if (this == &other) {
            return *this;
        }

        releaseMemory();
        _tracker = other._tracker;
        _curMemoryUsageBytes = other._curMemoryUsageBytes;
        other._tracker = nullptr;
        return *this;
    }

    ~MemoryUsageTokenImpl() {
        releaseMemory();
    }

    int64_t getCurrentMemoryUsageBytes() const {
        return _curMemoryUsageBytes;
    }

    const Tracker* tracker() const {
        return _tracker;
    }

    Tracker* tracker() {
        return _tracker;
    }

protected:
    void releaseMemory() {
        if (_tracker) {
            _tracker->add(-_curMemoryUsageBytes);
        }
    }

    Tracker* _tracker{nullptr};
    int64_t _curMemoryUsageBytes{0};
};

using MemoryUsageToken = MemoryUsageTokenImpl<SimpleMemoryUsageTracker>;
using SimpleMemoryUsageToken = MemoryUsageTokenImpl<SimpleMemoryUsageTracker>;

/**
 * Template to easy couple MemoryTokens with stored data.
 */
template <typename Tracker, typename T>
class MemoryUsageTokenWithImpl {
public:
    template <std::enable_if_t<std::is_default_constructible_v<T>, bool> = true>
    MemoryUsageTokenWithImpl() : _token(), _value() {}

    template <typename... Args>
    MemoryUsageTokenWithImpl(MemoryUsageTokenImpl<Tracker> token, Args&&... args)
        : _token(std::move(token)), _value(std::forward<Args>(args)...) {}

    MemoryUsageTokenWithImpl(const MemoryUsageTokenWithImpl&) = delete;
    MemoryUsageTokenWithImpl& operator=(const MemoryUsageTokenWithImpl&) = delete;

    MemoryUsageTokenWithImpl(MemoryUsageTokenWithImpl&& other) = default;
    MemoryUsageTokenWithImpl& operator=(MemoryUsageTokenWithImpl&& other) = default;

    const T& value() const {
        return _value;
    }
    T& value() {
        return _value;
    }

private:
    MemoryUsageTokenImpl<Tracker> _token;
    T _value;
};

template <typename T>
using MemoryUsageTokenWith = MemoryUsageTokenWithImpl<SimpleMemoryUsageTracker, T>;

template <typename T>
using SimpleMemoryUsageTokenWith = MemoryUsageTokenWithImpl<SimpleMemoryUsageTracker, T>;

}  // namespace mongo
