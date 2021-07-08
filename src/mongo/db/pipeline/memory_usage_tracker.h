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

#include <memory>
#include <utility>

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/str.h"

namespace mongo {

/**
 * This is a utility class for tracking memory usage across multiple arbitrary operators or
 * functions, which are identified by their string names.
 */
class MemoryUsageTracker {
public:
    class PerFunctionMemoryTracker {
    public:
        explicit PerFunctionMemoryTracker(MemoryUsageTracker* base) : base(base){};
        PerFunctionMemoryTracker() = delete;

        void update(long long diff) {
            tassert(5578603,
                    str::stream() << "Underflow on memory tracking, attempting to add " << diff
                                  << " but only " << _currentMemoryBytes << " available",
                    diff >= 0 || _currentMemoryBytes >= std::abs(diff));
            set(_currentMemoryBytes + diff);
        }

        void set(long long total) {
            if (total > _maxMemoryBytes)
                _maxMemoryBytes = total;
            long long prior = _currentMemoryBytes;
            _currentMemoryBytes = total;
            base->update(total - prior);
        }

        auto currentMemoryBytes() const {
            return _currentMemoryBytes;
        }

        auto maxMemoryBytes() const {
            return _maxMemoryBytes;
        }

        MemoryUsageTracker* base = nullptr;

    private:
        // Maximum memory consumption thus far observed for this function.
        long long _maxMemoryBytes = 0;
        // Tracks the current memory footprint.
        long long _currentMemoryBytes = 0;
    };

    MemoryUsageTracker(bool allowDiskUse = false, size_t maxMemoryUsageBytes = 0)
        : _allowDiskUse(allowDiskUse), _maxAllowedMemoryUsageBytes(maxMemoryUsageBytes) {}

    /**
     * Sets the new total for 'name', and updates the current total memory usage.
     */
    void set(StringData name, long long total) {
        (*this)[name].set(total);
    }

    /**
     * Sets the new current memory usage in bytes.
     */
    void set(long long total) {
        _memoryUsageBytes = total;
        if (_memoryUsageBytes > _maxMemoryUsageBytes) {
            _maxMemoryUsageBytes = _memoryUsageBytes;
        }
    }

    /**
     * Resets both the total memory usage as well as the per-function memory usage, but retains the
     * current value for maximum total memory usage.
     */
    void resetCurrent() {
        for (auto& [_, funcTracker] : _functionMemoryTracker) {
            funcTracker.set(0);
        }
        _memoryUsageBytes = 0;
    }

    /**
     * Provides read-only access to the function memory tracker for 'name'.
     */
    const PerFunctionMemoryTracker& operator[](StringData name) const {
        auto it = _functionMemoryTracker.find(_key(name));
        tassert(5466400,
                str::stream() << "Invalid call to memory usage tracker, could not find function "
                              << name,
                it != _functionMemoryTracker.end());
        return it->second;
    }

    /**
     * Non-const version, creates a new element if one doesn't exist and returns a reference to it.
     */
    PerFunctionMemoryTracker& operator[](StringData name) {
        auto [it, _] = _functionMemoryTracker.try_emplace(_key(name), this);
        return it->second;
    }

    /**
     * Updates the memory usage for 'name' by adding 'diff' to the current memory usage for
     * that function. Also updates the total memory usage.
     */
    void update(StringData name, long long diff) {
        (*this)[name].update(diff);
    }

    /**
     * Updates total memory usage.
     */
    void update(long long diff) {
        tassert(5578602,
                str::stream() << "Underflow on memory tracking, attempting to add " << diff
                              << " but only " << _memoryUsageBytes << " available",
                diff >= 0 || (int)_memoryUsageBytes >= -1 * diff);
        set(_memoryUsageBytes + diff);
    }

    auto currentMemoryBytes() const {
        return _memoryUsageBytes;
    }
    auto maxMemoryBytes() const {
        return _maxMemoryUsageBytes;
    }

    const bool _allowDiskUse;
    const size_t _maxAllowedMemoryUsageBytes;

private:
    static absl::string_view _key(StringData s) {
        return {s.rawData(), s.size()};
    }

    // Tracks current memory used.
    long long _memoryUsageBytes = 0;
    long long _maxMemoryUsageBytes = 0;

    // Tracks memory consumption per function using the output field name as a key.
    stdx::unordered_map<std::string, PerFunctionMemoryTracker> _functionMemoryTracker;
};

}  // namespace mongo
