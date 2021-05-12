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

#include "mongo/util/string_map.h"

namespace mongo {

/**
 * This is a utility class for tracking memory usage across multiple arbitrary operators or
 * functions, which are identified by their string names.
 */
class MemoryUsageTracker {
public:
    class PerFunctionMemoryTracker {
    public:
        PerFunctionMemoryTracker() = default;

        void update(int diff) {
            _currentMemoryBytes += diff;
            if (_currentMemoryBytes > _maxMemoryBytes)
                _maxMemoryBytes = _currentMemoryBytes;
        }

        void set(uint64_t total) {
            if (total > _maxMemoryBytes)
                _maxMemoryBytes = total;
            _currentMemoryBytes = total;
        }

        auto currentMemoryBytes() const {
            return _currentMemoryBytes;
        }

        auto maxMemoryBytes() const {
            return _maxMemoryBytes;
        }

    private:
        // Maximum memory consumption thus far observed for this function.
        uint64_t _maxMemoryBytes = 0;
        // Tracks the current memory footprint.
        uint64_t _currentMemoryBytes = 0;
    };

    MemoryUsageTracker(bool allowDiskUse, size_t maxMemoryUsageBytes)
        : _allowDiskUse(allowDiskUse), _maxAllowedMemoryUsageBytes(maxMemoryUsageBytes) {}

    /**
     * Sets the new total for 'functionName', and updates the current total memory usage.
     */
    void set(StringData functionName, uint64_t total) {
        auto oldFuncUsage = _functionMemoryTracker[functionName].currentMemoryBytes();
        _functionMemoryTracker[functionName].set(total);
        update(total - oldFuncUsage);
    }

    void setInternal(StringData functionName, uint64_t total) {
        auto oldFuncUsage = _internalMemoryTracker[functionName].currentMemoryBytes();
        _internalMemoryTracker[functionName].set(total);
        _memoryUsageBytes += total - oldFuncUsage;
        if (_memoryUsageBytes > _maxMemoryUsageBytes) {
            _maxMemoryUsageBytes = _memoryUsageBytes;
        }
    }

    /**
     * Sets the new current memory usage in bytes.
     */
    void set(uint64_t total) {
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
        _memoryUsageBytes = 0;
        for (auto& [_, funcTracker] : _functionMemoryTracker) {
            funcTracker.set(0);
        }
        for (auto& [_, funcTracker] : _internalMemoryTracker) {
            funcTracker.set(0);
        }
    }

    /**
     * Provides read-only access to the function memory tracker for 'name'.
     */
    auto operator[](StringData name) const {
        tassert(5466400,
                str::stream() << "Invalid call to memory usage tracker, could not find function "
                              << name,
                _functionMemoryTracker.find(name) != _functionMemoryTracker.end());
        return _functionMemoryTracker.at(name);
        MONGO_UNREACHABLE;
    }

    auto readInternal(StringData name) const {
        tassert(5643009,
                str::stream() << "Invalid call to memory usage tracker, could not find function "
                              << name,
                _internalMemoryTracker.find(name) != _internalMemoryTracker.end());
        return _internalMemoryTracker.at(name);
    }

    /**
     * Updates the memory usage for 'functionName' by adding 'diff' to the current memory usage for
     * that function. Also updates the total memory usage.
     */
    void update(StringData name, int diff) {
        _functionMemoryTracker[name].update(diff);
        update(diff);
    }

    /**
     * Updates total memory usage.
     */
    void update(int diff) {
        set(_memoryUsageBytes + diff);
    }
    void updateInternal(StringData name, int diff) {
        _internalMemoryTracker[name].update(diff);
        _memoryUsageBytes += diff;
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
    // Tracks current memory used.
    size_t _memoryUsageBytes = 0;
    size_t _maxMemoryUsageBytes = 0;

    // Tracks memory consumption per function using the output field name as a key.
    StringMap<PerFunctionMemoryTracker> _functionMemoryTracker;
    // Tracks memory consumption of internal values so there is no worry of colliding with a user
    // field name.
    StringMap<PerFunctionMemoryTracker> _internalMemoryTracker;
};

}  // namespace mongo
