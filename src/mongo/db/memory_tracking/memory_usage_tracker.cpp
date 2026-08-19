// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

#include <absl/strings/string_view.h>
#include <fmt/format.h>

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

MONGO_COMPILER_NORETURN void memoryTrackingUnderflowFailed(int64_t diff, int64_t available);

// Intentionally out-of-line here and not inlined as a simple 'tassert()' inside 'addInternal()' for
// performance reasons. Re-inling the function into 'addInternal()' may result in a performance
// degradation on some platforms.
MONGO_COMPILER_NOINLINE void memoryTrackingUnderflowFailed(int64_t diff, int64_t available) {
    tasserted(6128100,
              fmt::format("Underflow in memory tracking, "
                          "attempting to add {} but only {} available",
                          diff,
                          available));
}

}  // namespace

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker(SimpleMemoryUsageTracker* base,
                                                   MemoryUsageLimit maxAllowedMemoryUsageBytes,
                                                   int64_t chunkSize)
    : _base(base),
      _chunkSize(chunkSize),
      _maxAllowedMemoryUsageBytes(std::move(maxAllowedMemoryUsageBytes)) {}

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker(MemoryUsageLimit maxAllowedMemoryUsageBytes,
                                                   int64_t chunkSize)
    : SimpleMemoryUsageTracker(nullptr, std::move(maxAllowedMemoryUsageBytes), chunkSize) {}

SimpleMemoryUsageTracker::SimpleMemoryUsageTracker()
    : SimpleMemoryUsageTracker(MemoryUsageLimit{std::numeric_limits<int64_t>::max()}) {}

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
    : _chunkSize(chunkSize), _reportCallback(std::move(callback)) {
    tassert(11114200, "Expected positive value for chunkSize", _chunkSize > 0);
}

void SimpleMemoryUsageTracker::addInternal(int64_t diff, bool report) {
    // The public 'add()' only calls internal 'addInternal()' function for diff values != 0.
    dassert(diff != 0);

    int64_t inUse = _inUseTrackedMemoryBytes + diff;
    _inUseTrackedMemoryBytes = inUse;
    if (MONGO_unlikely(inUse < 0)) {
        memoryTrackingUnderflowFailed(diff, inUse - diff);
    }
    _peakTrackedMemoryBytes = std::max(_peakTrackedMemoryBytes, inUse);

    // When chunking is enabled we report to CurOp only when usage crosses a chunk boundary (0,
    // chunkSize, 2*chunkSize, ...), and also whenever it returns to zero so CurOp does not keep a
    // stale value once all memory is released. This avoids the lock contention of touching CurOp's
    // atomics on every update. The chunk size may live on an intermediate tracker rather than the
    // root, so the decision is computed here and carried in 'report' to the root, which performs
    // the CurOp write.
    if (_chunkSize) {
        // '_lastReportedLowerBound' is only ever assigned a multiple of '_chunkSize', and 'inUse'
        // is known to be non-negative here, so 'offsetSinceLastReported' locates 'inUse' relative
        // to the chunk we last reported: [0, chunkSize) is that same chunk, and anything else is a
        // boundary crossing.
        //
        // Note this fires at one level of the chain only: the per-accumulator child trackers
        // ('MemoryUsageTracker::operator[]') and the root operation tracker are both built with
        // 'chunkSize == 0', so only a stage's own base tracker gets here.
        const int64_t offsetSinceLastReported = inUse - _lastReportedLowerBound;

        if (static_cast<uint64_t>(offsetSinceLastReported) < static_cast<uint64_t>(_chunkSize)) {
            // 'diff' is always non-zero, as add() is the only entry point and it filters those
            // out, so returning to zero always means a transition to zero.
            report = (inUse == 0);
        } else {
            // A crossing, so the bound moves and a report is always due. ('inUse == 0' is included:
            // zero is only outside the chunk last reported when that chunk was not the one starting
            // at zero, and the return-to-zero case inside that chunk is handled above.)
            //
            // Stepping the bound by one chunk instead of dividing was measured here and did not
            // pay: it trades the divide for data-dependent branches, which is a wash even when
            // every update crosses. Not worth the extra code.
            _lastReportedLowerBound = (inUse / _chunkSize) * _chunkSize;
            report = true;
        }
    }

    if (_base) {
        _base->addInternal(diff, report);
    } else if (report && _writeToCurOp) {
        // 'report' is tested first so that the common non-reporting case does not have to load
        // the std::function's target pointer.
        reportToCurOp();
    }
}

// This function is often used on the hot path of memory tracking. Don't add an unnecessary stack
// protector to save a few instructions here. The function only spills two int64_t values and passes
// their addresses to the callback function.
MONGO_COMPILER_NOINLINE MONGO_COMPILER_NO_STACK_PROTECTOR void
SimpleMemoryUsageTracker::reportToCurOp() const {
    _writeToCurOp(_inUseTrackedMemoryBytes, _peakTrackedMemoryBytes);
}

void SimpleMemoryUsageTracker::resetBase(SimpleMemoryUsageTracker* base) {
    // Move our contribution off the old base and onto the new one so the ancestor chain's totals
    // stay consistent. When old and new are the same object (e.g. the operation tracker was carried
    // across a getMore) this nets to zero rather than double-counting.
    if (_base) {
        _base->add(-_inUseTrackedMemoryBytes);
    }
    _base = base;
    if (_base) {
        _base->add(_inUseTrackedMemoryBytes);
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

void SimpleMemoryUsageTracker::uassertedMemoryLimitExceeded(OperationContext* opCtx,
                                                            std::string_view name,
                                                            std::string_view stageName) const {
    str::stream msg;
    msg << name << " needs too much memory.";
    if (!stageName.empty()) {
        msg << " Stage: " << stageName << ".";
    }
    msg << " Needs: " << _inUseTrackedMemoryBytes
        << " bytes. Local memory limit: " << _maxAllowedMemoryUsageBytes.get(opCtx) << " bytes.";
    int level = 1;
    for (const SimpleMemoryUsageTracker* current = _base; current; current = current->_base) {
        if (current->_base) {
            msg << " Level " << level << " memory used: " << current->inUseTrackedMemoryBytes()
                << " bytes. Level " << level
                << " memory limit: " << current->maxAllowedMemoryUsageBytes(opCtx) << " bytes.";
            ++level;
        } else {
            msg << " Global memory used: " << current->inUseTrackedMemoryBytes()
                << " bytes. Global memory limit: " << current->maxAllowedMemoryUsageBytes(opCtx)
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
