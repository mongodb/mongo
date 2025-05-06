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

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <absl/strings/string_view.h>
#include <algorithm>
#include <boost/noncopyable.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

/**
 * This is a utility class for tracking memory usage across multiple arbitrary operators or
 * functions, which are identified by their string names. Tracks both current and highest
 * encountered memory consumption,
 *
 * It can be used directly by calling MemoryUsageTracker::add(int64_t diff), or by creating a
 * dependent tracker via MemoryUsageTracker::operator[].
 *
 * Dependent tracker will update both it's own memory and the total. It is used to tracker the
 * consumption of individual parts, such as different accumulators in $group, while simulaniously
 * keeping track of the total.
 *
 * Cannot be shallow copied because child memory trackers point to the address of the inline
 * base tracker of the class.
 *
 * TODO SERVER-80007: move implementation to .cpp to save on compilation time.
 */
class MemoryUsageTracker {
public:
    /**
     * The class that does the tracking. Instances should be created via
     * MemoryUsageTracker::operator[].
     */
    class Impl {
    public:
        Impl(Impl* base, int64_t maxAllowedMemoryUsageBytes)
            : _base(base), _maxAllowedMemoryUsageBytes(maxAllowedMemoryUsageBytes){};

        void add(int64_t diff) {
            _currentMemoryBytes += diff;
            tassert(6128100,
                    str::stream() << "Underflow in memory tracking, attempting to add " << diff
                                  << " but only " << _currentMemoryBytes - diff << " available",
                    _currentMemoryBytes >= 0);
            if (_currentMemoryBytes > _maxMemoryBytes) {
                _maxMemoryBytes = _currentMemoryBytes;
            }
            if (_base) {
                _base->add(diff);
            }
        }

        void set(int64_t total) {
            add(total - _currentMemoryBytes);
        }

        int64_t currentMemoryBytes() const {
            return _currentMemoryBytes;
        }

        int64_t maxMemoryBytes() const {
            return _maxMemoryBytes;
        }

        bool withinMemoryLimit() const {
            return _currentMemoryBytes <= _maxAllowedMemoryUsageBytes;
        }

        int64_t maxAllowedMemoryUsageBytes() const {
            return _maxAllowedMemoryUsageBytes;
        }

    private:
        Impl* _base = nullptr;

        // Maximum memory consumption thus far observed for this function.
        int64_t _maxMemoryBytes = 0;
        // Tracks the current memory footprint.
        int64_t _currentMemoryBytes = 0;

        int64_t _maxAllowedMemoryUsageBytes;
    };

    MemoryUsageTracker(bool allowDiskUse = false, int64_t maxMemoryUsageBytes = 0)
        : _allowDiskUse(allowDiskUse), _baseTracker(nullptr, maxMemoryUsageBytes) {}

    /**
     * Sets the new total for 'name', and updates the current total memory usage.
     */
    void set(StringData name, int64_t total) {
        (*this)[name].set(total);
    }

    /**
     * Resets both the total memory usage as well as the per-function memory usage, but retains the
     * current value for maximum total memory usage.
     */
    void resetCurrent() {
        for (auto& [_, funcTracker] : _functionMemoryTracker) {
            funcTracker.set(0);
        }
        _baseTracker.set(0);
    }

    /**
     * Clears the child memory trackers map and resets the base tracker memory usage to zero.
     */
    void clear() {
        _functionMemoryTracker.clear();
        resetCurrent();
    }

    /**
     * Non-const version, creates a new element if one doesn't exist and returns a reference to it.
     */
    Impl& operator[](StringData name) {
        auto [it, _] = _functionMemoryTracker.try_emplace(
            _key(name), &_baseTracker, _baseTracker.maxAllowedMemoryUsageBytes());
        return it->second;
    }

    /**
     * Updates the memory usage for 'name' by adding 'diff' to the current memory usage for
     * that function. Also updates the total memory usage.
     */
    void add(StringData name, int64_t diff) {
        (*this)[name].add(diff);
    }

    /**
     * Updates total memory usage.
     */
    void add(int64_t diff) {
        _baseTracker.add(diff);
    }

    auto currentMemoryBytes() const {
        return _baseTracker.currentMemoryBytes();
    }
    auto maxMemoryBytes() const {
        return _baseTracker.maxMemoryBytes();
    }

    auto maxMemoryBytes(StringData name) const {
        const auto it = _functionMemoryTracker.find(_key(name));
        return it == _functionMemoryTracker.end() ? 0 : it->second.maxMemoryBytes();
    }

    bool withinMemoryLimit() const {
        return _baseTracker.withinMemoryLimit();
    }

    bool allowDiskUse() const {
        return _allowDiskUse;
    }

    int64_t maxAllowedMemoryUsageBytes() const {
        return _baseTracker.maxAllowedMemoryUsageBytes();
    }

private:
    Impl& _impl() {
        return _baseTracker;
    }

    static absl::string_view _key(StringData s) {
        return {s.rawData(), s.size()};
    }

    bool _allowDiskUse;
    // Tracks current memory used.
    Impl _baseTracker;
    // Tracks memory consumption per function using the output field name as a key.
    stdx::unordered_map<std::string, Impl> _functionMemoryTracker;
};

// Lightweight version of memory usage tracker for use cases where we don't need historical maximum
// and per-function memory tracking.
class SimpleMemoryUsageTracker {
public:
    SimpleMemoryUsageTracker(int64_t maxAllowedMemoryUsageBytes)
        : _maxAllowedMemoryUsageBytes(maxAllowedMemoryUsageBytes){};

    void set(int64_t value) {
        _currentMemoryBytes = value;
    }

    void add(int64_t diff) {
        _currentMemoryBytes += diff;
        tassert(6128101,
                str::stream() << "Underflow in memory tracking, attempting to add " << diff
                              << " but only " << _currentMemoryBytes - diff << " available",
                _currentMemoryBytes >= 0);
    }

    int64_t currentMemoryBytes() const {
        return _currentMemoryBytes;
    }

    int64_t maxAllowedMemoryUsageBytes() const {
        return _maxAllowedMemoryUsageBytes;
    }

    bool withinMemoryLimit() const {
        return _currentMemoryBytes <= _maxAllowedMemoryUsageBytes;
    }

private:
    int64_t _currentMemoryBytes = 0;
    const int64_t _maxAllowedMemoryUsageBytes;
};

/**
 * An RAII utility class which can make it easy to account for some new allocation in a given
 * 'MemoryUsageTracker' for the entire lifetime of the object. This is meant to be a static
 * allocation that is released when it goes out of scope, for a dynamic instance use
 * `MemoryUsageHandle`
 */
template <typename Tracker>
class MemoryUsageTokenImpl : private boost::noncopyable {
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

using MemoryUsageToken = MemoryUsageTokenImpl<MemoryUsageTracker::Impl>;
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
using MemoryUsageTokenWith = MemoryUsageTokenWithImpl<MemoryUsageTracker::Impl, T>;

template <typename T>
using SimpleMemoryUsageTokenWith = MemoryUsageTokenWithImpl<SimpleMemoryUsageTracker, T>;

// Updatable version of `MemoryUsageTokenImpl`.
template <typename Tracker>
class MemoryUsageHandleImpl : public MemoryUsageTokenImpl<Tracker> {
public:
    MemoryUsageHandleImpl(Tracker* tracker = nullptr) : MemoryUsageTokenImpl<Tracker>(0, tracker) {}
    MemoryUsageHandleImpl(int64_t initial, Tracker* tracker = nullptr)
        : MemoryUsageTokenImpl<Tracker>(initial, tracker) {}

    void add(int64_t diff) {
        this->_curMemoryUsageBytes += diff;
        this->_tracker->add(diff);
    }

    void set(int64_t total) {
        add(total - this->_curMemoryUsageBytes);
    }
};  // class MemoryUsageHandleImpl

using SimpleMemoryUsageHandle = MemoryUsageHandleImpl<SimpleMemoryUsageTracker>;

}  // namespace mongo
