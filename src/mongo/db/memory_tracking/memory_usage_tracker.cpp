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

#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <limits>
#include <string>
#include <string_view>

#include <absl/strings/string_view.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

absl::string_view toKey(std::string_view s) {
    return {s.data(), s.size()};
}

// Number of times a query operation was failed with an ExceededMemoryLimit error because it
// exceeded a memory-tracking limit. This is a single metric exposed on two surfaces: the
// `serverStatusOptions` below publishes it in serverStatus as
// `metrics.query.operationsFailedDueToMemoryLimit`, and the same value is exported over
// OpenTelemetry.
auto& operationsFailedDueToMemoryLimit =
    otel::metrics::MetricsService::instance().createInt64Counter(
        otel::metrics::MetricNames::kQueryOperationsFailedDueToMemoryLimit,
        "Number of query operations failed because they exceeded a memory-tracking limit",
        otel::metrics::MetricUnit::kOperations,
        {.serverStatusOptions = otel::metrics::ServerStatusOptions{
             .dottedPath = "query.operationsFailedDueToMemoryLimit", .role = ClusterRole::None}});

}  // namespace

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker(SimpleMemoryUsageTracker* base,
                                                   MemoryUsageLimit maxAllowedMemoryUsageBytes,
                                                   int64_t chunkSize)
    : _base(base),
      _maxAllowedMemoryUsageBytes(std::move(maxAllowedMemoryUsageBytes)),
      _chunkSize(chunkSize) {}

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker(MemoryUsageLimit maxAllowedMemoryUsageBytes,
                                                   int64_t chunkSize)
    : SimpleMemoryUsageTracker(nullptr, std::move(maxAllowedMemoryUsageBytes), chunkSize) {}

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker()
    : SimpleMemoryUsageTracker(MemoryUsageLimit{std::numeric_limits<int64_t>::max()}) {}

void SimpleMemoryUsageTracker::set(int64_t total) {
    add(total - _inUseTrackedMemoryBytes);
}

void SimpleMemoryUsageTracker::setWriteToCurOp(std::function<void(int64_t, int64_t)> writeToCurOp) {
    _writeToCurOp = std::move(writeToCurOp);
}

MemoryUsageTracker::MemoryUsageTracker(SimpleMemoryUsageTracker* baseParent,
                                       bool allowDiskUse,
                                       MemoryUsageLimit maxMemoryUsageBytes,
                                       int64_t chunkSize)
    : _allowDiskUse(allowDiskUse),
      _baseTracker(baseParent, std::move(maxMemoryUsageBytes), chunkSize) {}

MemoryUsageTracker::MemoryUsageTracker(bool allowDiskUse, MemoryUsageLimit maxMemoryUsageBytes)
    : MemoryUsageTracker(nullptr, allowDiskUse, std::move(maxMemoryUsageBytes)) {}

void MemoryUsageTracker::set(std::string_view name, int64_t total) {
    (*this)[name].set(total);
}

void MemoryUsageTracker::add(std::string_view name, int64_t diff) {
    (*this)[name].add(diff);
}


DeduplicatorReporter::DeduplicatorReporter(std::function<void(int64_t, int64_t)> callback,
                                           int64_t chunkSize)
    : _reportCallback(std::move(callback)), _chunkSize(chunkSize) {
    tassert(11114200, "Expected positive value for chunkSize", _chunkSize > 0);
}

void SimpleMemoryUsageTracker::add(int64_t diff) {
    addInternal(diff, true /* report */);
}

void SimpleMemoryUsageTracker::addInternal(int64_t diff, bool report) {
    _inUseTrackedMemoryBytes += diff;
    tassert(6128100,
            str::stream() << "Underflow in memory tracking, attempting to add " << diff
                          << " but only " << _inUseTrackedMemoryBytes - diff << " available",
            _inUseTrackedMemoryBytes >= 0);
    if (_inUseTrackedMemoryBytes > _peakTrackedMemoryBytes) {
        _peakTrackedMemoryBytes = _inUseTrackedMemoryBytes;
    }

    // When chunking is enabled we report to CurOp only when usage crosses a chunk boundary (0,
    // chunkSize, 2*chunkSize, ...), and also whenever it returns to zero so CurOp does not keep a
    // stale value once all memory is released. This avoids the lock contention of touching CurOp's
    // atomics on every update. The chunk size may live on an intermediate tracker rather than the
    // root, so the decision is computed here and carried in 'report' to the root, which performs
    // the CurOp write.
    if (_chunkSize) {
        int64_t newLowerBound = (_inUseTrackedMemoryBytes / _chunkSize) * _chunkSize;
        report = newLowerBound != _lastReportedLowerBound ||
            (_inUseTrackedMemoryBytes == 0 && diff != 0);
        if (report) {
            _lastReportedLowerBound = newLowerBound;
        }
    }

    if (_base) {
        _base->addInternal(diff, report);
    } else if (_writeToCurOp && report) {
        _writeToCurOp(_inUseTrackedMemoryBytes, _peakTrackedMemoryBytes);
    }
}

SimpleMemoryUsageTracker SimpleMemoryUsageTracker::makeFreshSimpleMemoryUsageTracker() const {
    // Copy the limit holder itself rather than a resolved byte count, so that any future
    // lazily-resolved limit stays lazy in the fresh tracker.
    SimpleMemoryUsageTracker memTracker =
        SimpleMemoryUsageTracker{_base, _maxAllowedMemoryUsageBytes, _chunkSize};
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

SimpleMemoryUsageTracker& MemoryUsageTracker::operator[](std::string_view name) {
    auto [it, _] = _functionMemoryTracker.try_emplace(
        toKey(name), &_baseTracker, _baseTracker.maxAllowedMemoryUsageLimit());
    return it->second;
}

int64_t MemoryUsageTracker::peakTrackedMemoryBytes(std::string_view name) const {
    const auto it = _functionMemoryTracker.find(toKey(name));
    return it == _functionMemoryTracker.end() ? 0 : it->second.peakTrackedMemoryBytes();
}

void MemoryUsageTracker::assertCanSpill(std::string_view name) const {
    _baseTracker.assertCanSpill(_allowDiskUse, name);
}

MemoryUsageTracker MemoryUsageTracker::makeFreshMemoryUsageTracker() const {
    return MemoryUsageTracker(
        _baseTracker._base, allowDiskUse(), _baseTracker.maxAllowedMemoryUsageLimit());
}

void DeduplicatorReporter::add(int64_t bytesDiff, int64_t recordsDiff) {

    _inUseTrackedMemoryBytes += bytesDiff;
    _inUseRecordIdCount += recordsDiff;
    tassert(12579700,
            str::stream() << "Underflow in record count tracking, attempting to add " << recordsDiff
                          << " but only " << _inUseRecordIdCount - recordsDiff << " available",
            _inUseRecordIdCount >= 0);
    tassert(12579701,
            str::stream() << "Underflow in memory tracking, attempting to add " << bytesDiff
                          << " but only " << _inUseTrackedMemoryBytes - bytesDiff << " available",
            _inUseTrackedMemoryBytes >= 0);

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

void SimpleMemoryUsageTracker::assertWithinMemoryLimit(std::string_view name,
                                                       std::string_view stageName) const {
    if (withinMemoryLimit()) {
        return;
    }
    str::stream msg;
    msg << name << " needs too much memory.";
    if (!stageName.empty()) {
        msg << " Stage: " << stageName << ".";
    }
    msg << " Needs: " << _inUseTrackedMemoryBytes
        << " bytes. Local memory limit: " << _maxAllowedMemoryUsageBytes.get() << " bytes.";
    int level = 1;
    for (const SimpleMemoryUsageTracker* current = _base; current; current = current->_base) {
        if (current->_base) {
            msg << " Level " << level << " memory used: " << current->inUseTrackedMemoryBytes()
                << " bytes. Level " << level
                << " memory limit: " << current->maxAllowedMemoryUsageBytes() << " bytes.";
            ++level;
        } else {
            msg << " Global memory used: " << current->inUseTrackedMemoryBytes()
                << " bytes. Global memory limit: " << current->maxAllowedMemoryUsageBytes()
                << " bytes.";
        }
    }
    std::string errmsg = msg;
    LOGV2_ERROR(12932700, "Query exceeded the memory limit", "error"_attr = errmsg);
    operationsFailedDueToMemoryLimit.add(1);
    uasserted(ErrorCodes::ExceededMemoryLimit, errmsg);
}

void SimpleMemoryUsageTracker::assertCanSpill(bool canSpill, std::string_view name) const {
    if (canSpill) {
        return;
    }

    // We are over memory limit and cannot spill; assert an error
    str::stream msg;
    msg << "Exceeded memory limit";
    if (!name.empty()) {
        msg << " for " << name;
    }
    msg << ", but didn't allow external spilling; pass allowDiskUse:true to opt in";

    std::string errmsg = msg;
    operationsFailedDueToMemoryLimit.add(1);
    uasserted(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed, errmsg);
}

}  // namespace mongo
