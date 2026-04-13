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

#include "mongo/db/memory_tracking/memory_usage_tracker.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <limits>

#include <absl/strings/string_view.h>

namespace mongo {
namespace {

absl::string_view toKey(StringData s) {
    return {s.data(), s.size()};
}

}  // namespace

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker(SimpleMemoryUsageTracker* base,
                                                   int64_t maxAllowedMemoryUsageBytes,
                                                   int64_t chunkSize)
    : _base(base), _maxAllowedMemoryUsageBytes(maxAllowedMemoryUsageBytes), _chunkSize(chunkSize) {}

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker(int64_t maxAllowedMemoryUsageBytes,
                                                   int64_t chunkSize)
    : SimpleMemoryUsageTracker(nullptr, maxAllowedMemoryUsageBytes, chunkSize) {}

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker()
    : SimpleMemoryUsageTracker(std::numeric_limits<int64_t>::max()) {}

void SimpleMemoryUsageTracker::set(int64_t total) {
    add(total - _inUseTrackedMemoryBytes);
}

void SimpleMemoryUsageTracker::setWriteToCurOp(std::function<void(int64_t, int64_t)> writeToCurOp) {
    _writeToCurOp = std::move(writeToCurOp);
}

MemoryUsageTracker::MemoryUsageTracker(SimpleMemoryUsageTracker* baseParent,
                                       bool allowDiskUse,
                                       int64_t maxMemoryUsageBytes,
                                       int64_t chunkSize)
    : _allowDiskUse(allowDiskUse), _baseTracker(baseParent, maxMemoryUsageBytes, chunkSize) {}

MemoryUsageTracker::MemoryUsageTracker(bool allowDiskUse, int64_t maxMemoryUsageBytes)
    : MemoryUsageTracker(nullptr, allowDiskUse, maxMemoryUsageBytes) {}

void MemoryUsageTracker::set(StringData name, int64_t total) {
    (*this)[name].set(total);
}

void MemoryUsageTracker::add(StringData name, int64_t diff) {
    (*this)[name].add(diff);
}


DeduplicatorReporter::DeduplicatorReporter(std::function<void(int64_t, int64_t)> callback,
                                           int64_t chunkSize)
    : _reportCallback(std::move(callback)), _chunkSize(chunkSize) {
    tassert(11114200, "Expected positive value for chunkSize", _chunkSize > 0);
}

void SimpleMemoryUsageTracker::add(int64_t diff) {
    _inUseTrackedMemoryBytes += diff;
    tassert(6128100,
            str::stream() << "Underflow in memory tracking, attempting to add " << diff
                          << " but only " << _inUseTrackedMemoryBytes - diff << " available",
            _inUseTrackedMemoryBytes >= 0);
    if (_inUseTrackedMemoryBytes > _peakTrackedMemoryBytes) {
        _peakTrackedMemoryBytes = _inUseTrackedMemoryBytes;
    }

    // When chunking is enabled, we report memory usage in discrete chunks (0, chunkSize,
    // 2*chunkSize, ...) rather than exact values. This reduces update frequency and
    // provides predictable lower-bound semantics where CurOp's reported value <= actual
    // usage < reported value + chunkSize.
    //
    // This is to avoid performance regressions, but will also result having slightly less
    // accurate statistics in CurOp.
    int64_t inUseTrackedMemoryBytes = _inUseTrackedMemoryBytes;

    if (_base) {
        if (_chunkSize) {
            int64_t newLowerBound = (_inUseTrackedMemoryBytes / _chunkSize) * _chunkSize;

            if (newLowerBound != _lastReportedLowerBound) {
                int64_t chunkedDelta = newLowerBound - _lastReportedLowerBound;

                _base->add(chunkedDelta);
                _lastReportedLowerBound = newLowerBound;
                inUseTrackedMemoryBytes = newLowerBound;
            }
        } else {
            _base->add(diff);
        }
    } else if (_writeToCurOp) {
        _writeToCurOp(inUseTrackedMemoryBytes, _peakTrackedMemoryBytes);
    }
}

SimpleMemoryUsageTracker SimpleMemoryUsageTracker::makeFreshSimpleMemoryUsageTracker() const {
    SimpleMemoryUsageTracker memTracker =
        SimpleMemoryUsageTracker{_base, maxAllowedMemoryUsageBytes(), _chunkSize};
    memTracker.setWriteToCurOp(_writeToCurOp);
    return memTracker;
}

void MemoryUsageTracker::resetCurrent() {
    for (auto& [_, funcTracker] : _functionMemoryTracker) {
        funcTracker.set(0);
    }
    _baseTracker.set(0);
}

void MemoryUsageTracker::clear() {
    _functionMemoryTracker.clear();
    resetCurrent();
}

SimpleMemoryUsageTracker& MemoryUsageTracker::operator[](StringData name) {
    auto [it, _] = _functionMemoryTracker.try_emplace(
        toKey(name), &_baseTracker, _baseTracker.maxAllowedMemoryUsageBytes());
    return it->second;
}

int64_t MemoryUsageTracker::peakTrackedMemoryBytes(StringData name) const {
    const auto it = _functionMemoryTracker.find(toKey(name));
    return it == _functionMemoryTracker.end() ? 0 : it->second.peakTrackedMemoryBytes();
}

MemoryUsageTracker MemoryUsageTracker::makeFreshMemoryUsageTracker() const {
    return MemoryUsageTracker(_baseTracker._base, allowDiskUse(), maxAllowedMemoryUsageBytes());
}

void DeduplicatorReporter::add(int64_t diff) {
    _inUseTrackedMemoryBytes += diff;
    _inUseRecordIdCount++;

    // When chunking is enabled, we report memory usage in discrete chunks (0, chunkSize,
    // 2*chunkSize, ...) rather than exact values.
    // This is to avoid performance regressions, but will also result having slightly less
    // accurate statistics in serverStatus.
    int64_t newLowerBound = (_inUseTrackedMemoryBytes / _chunkSize) * _chunkSize;

    // Nothing to report, early exit.
    if (newLowerBound == _lastReportedLowerBound) {
        return;
    }

    if (_reportCallback) {
        int64_t chunkedDelta = newLowerBound - _lastReportedLowerBound;
        int64_t recordIdDelta = _inUseRecordIdCount - _lastReportedRecordIdCount;
        _lastReportedLowerBound = newLowerBound;
        _lastReportedRecordIdCount = _inUseRecordIdCount;
        _reportCallback(chunkedDelta, recordIdDelta);
    }
}

}  // namespace mongo
