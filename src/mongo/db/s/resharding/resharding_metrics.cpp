/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include <algorithm>
#include <memory>

#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/duration.h"
#include "mongo/util/integer_histogram.h"
#include "mongo/util/with_alignment.h"

namespace mongo {

namespace {
constexpr auto kAnotherOperationInProgress = "Another operation is in progress";
constexpr auto kNoOperationInProgress = "No operation is in progress";
constexpr auto kMetricsSetBeforeRestore = "Expected metrics to be 0 prior to restore";

constexpr auto kTotalOps = "countReshardingOperations";
constexpr auto kSuccessfulOps = "countReshardingSuccessful";
constexpr auto kFailedOps = "countReshardingFailures";
constexpr auto kCanceledOps = "countReshardingCanceled";
constexpr auto kOpTimeElapsed = "totalOperationTimeElapsedSecs";
constexpr auto kOpTimeRemaining = "remainingOperationTimeEstimatedSecs";
constexpr auto kDocumentsToCopy = "approxDocumentsToCopy";
constexpr auto kDocumentsCopied = "documentsCopied";
constexpr auto kBytesToCopy = "approxBytesToCopy";
constexpr auto kBytesCopied = "bytesCopied";
constexpr auto kCopyTimeElapsed = "totalCopyTimeElapsedSecs";
constexpr auto kOplogsFetched = "oplogEntriesFetched";
constexpr auto kOplogsApplied = "oplogEntriesApplied";
constexpr auto kApplyTimeElapsed = "totalApplyTimeElapsedSecs";
constexpr auto kWritesDuringCritialSection = "countWritesDuringCriticalSection";
constexpr auto kCriticalSectionTimeElapsed = "totalCriticalSectionTimeElapsedSecs";
constexpr auto kCoordinatorState = "coordinatorState";
constexpr auto kDonorState = "donorState";
constexpr auto kRecipientState = "recipientState";
constexpr auto kOpStatus = "opStatus";
constexpr auto kLastOpEndingChunkImbalance = "lastOpEndingChunkImbalance";
constexpr auto kOpCounters = "opcounters";
constexpr auto kMinRemainingOperationTime = "minShardRemainingOperationTimeEstimatedMillis";
constexpr auto kMaxRemainingOperationTime = "maxShardRemainingOperationTimeEstimatedMillis";
constexpr auto kOplogApplierApplyBatchLatencyMillis = "oplogApplierApplyBatchLatencyMillis";
constexpr auto kCollClonerFillBatchForInsertLatencyMillis =
    "collClonerFillBatchForInsertLatencyMillis";

using MetricsPtr = std::unique_ptr<ReshardingMetrics>;

const auto getMetrics = ServiceContext::declareDecoration<MetricsPtr>();

const auto reshardingMetricsRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ReshardingMetrics",
    [](ServiceContext* ctx) { getMetrics(ctx) = std::make_unique<ReshardingMetrics>(ctx); }};

/**
 * Given a constant rate of time per unit of work:
 *    totalTime / totalWork == elapsedTime / elapsedWork
 * Solve for remaining time.
 *    remainingTime := totalTime - elapsedTime
 *                  == (totalWork * (elapsedTime / elapsedWork)) - elapsedTime
 *                  == elapsedTime * (totalWork / elapsedWork - 1)
 */
Milliseconds remainingTime(Milliseconds elapsedTime, double elapsedWork, double totalWork) {
    elapsedWork = std::min(elapsedWork, totalWork);
    double remainingMsec = 1.0 * elapsedTime.count() * (totalWork / elapsedWork - 1);
    return Milliseconds(Milliseconds::rep(remainingMsec));
}

void appendHistogram(BSONObjBuilder* bob,
                     const IntegerHistogram<kLatencyHistogramBucketsCount>& hist) {
    BSONObjBuilder histogramBuilder;
    hist.append(histogramBuilder, false);
    bob->appendElements(histogramBuilder.obj());
}

static StringData serializeState(boost::optional<RecipientStateEnum> e) {
    return RecipientState_serializer(*e);
}

static StringData serializeState(boost::optional<DonorStateEnum> e) {
    return DonorState_serializer(*e);
}

static StringData serializeState(boost::optional<CoordinatorStateEnum> e) {
    return CoordinatorState_serializer(*e);
}

// Enables resharding to distinguish the ops it does to keep up with client writes from the
// client workload tracked in the globalOpCounters.
class ReshardingOpCounters {
public:
    void gotInserts(int n) noexcept {
        _checkWrap(&ReshardingOpCounters::_insert, n);
    }
    void gotInsert() noexcept {
        _checkWrap(&ReshardingOpCounters::_insert, 1);
    }
    void gotUpdate() noexcept {
        _checkWrap(&ReshardingOpCounters::_update, 1);
    }
    void gotDelete() noexcept {
        _checkWrap(&ReshardingOpCounters::_delete, 1);
    }

    BSONObj getObj() const noexcept {
        BSONObjBuilder b;
        b.append("insert", _insert.loadRelaxed());
        b.append("update", _update.loadRelaxed());
        b.append("delete", _delete.loadRelaxed());
        return b.obj();
    }

private:
    // Increment member `counter` by n, resetting all counters if it was > 2^60.
    void _checkWrap(CacheAligned<AtomicWord<long long>> ReshardingOpCounters::*counter, int n) {
        static constexpr auto maxCount = 1LL << 60;
        auto oldValue = (this->*counter).fetchAndAddRelaxed(n);
        if (oldValue > maxCount - n) {
            LOGV2(5776000,
                  "ReshardingOpCounters exceeded maximum value, resetting all to 0",
                  "insert"_attr = _insert.loadRelaxed(),
                  "update"_attr = _update.loadRelaxed(),
                  "delete"_attr = _delete.loadRelaxed());
            _insert.store(0);
            _update.store(0);
            _delete.store(0);
        }
    }

    CacheAligned<AtomicWord<long long>> _insert;
    CacheAligned<AtomicWord<long long>> _update;
    CacheAligned<AtomicWord<long long>> _delete;
};

// Allows tracking elapsed time for the resharding operation and its sub operations (e.g.,
// applying oplog entries).
class TimeInterval {
public:
    void start(Date_t d) noexcept {
        if (_start) {
            LOGV2_WARNING(5892600, "Resharding metrics already started, start() is a no-op");
            return;
        }
        _start = d;
    }

    void end(Date_t d) noexcept {
        invariant(_start, "Not started");
        if (_end) {
            LOGV2_WARNING(5892601, "Resharding metrics already ended, end() is a no-op");
            return;
        }
        _end = d;
    }

    // TODO Remove this function once all metrics classes can start from stepup.
    void forceEnd(Date_t d) noexcept {
        if (!_start)
            _start = d;
        end(d);
    }

    Milliseconds duration(Date_t now) const noexcept {
        return !_start ? Milliseconds(0) : ((!_end ? now : *_end) - *_start);
    }

private:
    boost::optional<Date_t> _start;
    boost::optional<Date_t> _end;
};

}  // namespace

class ReshardingMetrics::OperationMetrics {
public:
    void appendCurrentOpMetrics(BSONObjBuilder*, Role, Date_t now) const;

    boost::optional<Milliseconds> remainingOperationTime(Date_t now) const;

    void gotInserts(int n) noexcept;
    void gotInsert() noexcept;
    void gotUpdate() noexcept;
    void gotDelete() noexcept;

    TimeInterval runningOperation;
    ReshardingOperationStatusEnum opStatus = ReshardingOperationStatusEnum::kInactive;

    TimeInterval copyingDocuments;
    int64_t documentsToCopy = 0;
    int64_t documentsCopied = 0;
    int64_t bytesToCopy = 0;
    int64_t bytesCopied = 0;

    TimeInterval applyingOplogEntries;
    int64_t oplogEntriesFetched = 0;
    int64_t oplogEntriesApplied = 0;

    TimeInterval inCriticalSection;
    int64_t writesDuringCriticalSection = 0;

    int64_t chunkImbalanceCount = 0;

    IntegerHistogram<kLatencyHistogramBucketsCount> oplogApplierApplyBatchLatencyMillis =
        IntegerHistogram<kLatencyHistogramBucketsCount>(kOplogApplierApplyBatchLatencyMillis,
                                                        latencyHistogramBuckets);
    IntegerHistogram<kLatencyHistogramBucketsCount> collClonerFillBatchForInsertLatencyMillis =
        IntegerHistogram<kLatencyHistogramBucketsCount>(kCollClonerFillBatchForInsertLatencyMillis,
                                                        latencyHistogramBuckets);

    // The ops done by resharding to keep up with the client writes.
    ReshardingOpCounters opCounters;

    Milliseconds minRemainingOperationTime = Milliseconds(0);
    Milliseconds maxRemainingOperationTime = Milliseconds(0);

    boost::optional<DonorStateEnum> donorState;
    boost::optional<RecipientStateEnum> recipientState;
    boost::optional<CoordinatorStateEnum> coordinatorState;
};

void ReshardingMetrics::OperationMetrics::gotInserts(int n) noexcept {
    opCounters.gotInserts(n);
}

void ReshardingMetrics::OperationMetrics::gotInsert() noexcept {
    opCounters.gotInsert();
}

void ReshardingMetrics::OperationMetrics::gotUpdate() noexcept {
    opCounters.gotUpdate();
}

void ReshardingMetrics::OperationMetrics::gotDelete() noexcept {
    opCounters.gotDelete();
}

boost::optional<Milliseconds> ReshardingMetrics::OperationMetrics::remainingOperationTime(
    Date_t now) const {
    if (recipientState > RecipientStateEnum::kCloning && oplogEntriesFetched == 0) {
        return Milliseconds(0);
    }

    if (oplogEntriesApplied > 0 && oplogEntriesFetched > 0) {
        // All fetched oplogEntries must be applied. Some of them already have been.
        return remainingTime(
            applyingOplogEntries.duration(now), oplogEntriesApplied, oplogEntriesFetched);
    }
    if (bytesCopied > 0 && bytesToCopy > 0) {
        // Until the time to apply batches of oplog entries is measured, we assume that applying all
        // of them will take as long as copying did.
        return remainingTime(copyingDocuments.duration(now), bytesCopied, 2 * bytesToCopy);
    }
    return {};
}

void ReshardingMetrics::OperationMetrics::appendCurrentOpMetrics(BSONObjBuilder* bob,
                                                                 Role role,
                                                                 Date_t now) const {
    auto getElapsedTime = [&](const TimeInterval& interval) -> int64_t {
        return durationCount<Seconds>(interval.duration(now));
    };

    const auto remainingMsec = remainingOperationTime(now);

    bob->append(kOpTimeElapsed, getElapsedTime(runningOperation));

    switch (role) {
        case Role::kDonor:
            bob->append(kWritesDuringCritialSection, writesDuringCriticalSection);
            bob->append(kCriticalSectionTimeElapsed, getElapsedTime(inCriticalSection));
            bob->append(kDonorState,
                        serializeState(donorState.get_value_or(DonorStateEnum::kUnused)));
            bob->append(kOpStatus, ReshardingOperationStatus_serializer(opStatus));
            break;
        case Role::kRecipient:
            bob->append(kOpTimeRemaining,
                        !remainingMsec ? int64_t{-1} /** -1 is a specified integer null value */
                                       : durationCount<Seconds>(*remainingMsec));
            bob->append(kDocumentsToCopy, documentsToCopy);
            bob->append(kDocumentsCopied, documentsCopied);
            bob->append(kBytesToCopy, bytesToCopy);
            bob->append(kBytesCopied, bytesCopied);
            bob->append(kCopyTimeElapsed, getElapsedTime(copyingDocuments));

            bob->append(kOplogsFetched, oplogEntriesFetched);
            bob->append(kOplogsApplied, oplogEntriesApplied);
            bob->append(kApplyTimeElapsed, getElapsedTime(applyingOplogEntries));
            bob->append(kRecipientState,
                        serializeState(recipientState.get_value_or(RecipientStateEnum::kUnused)));
            bob->append(kOpStatus, ReshardingOperationStatus_serializer(opStatus));

            appendHistogram(bob, oplogApplierApplyBatchLatencyMillis);
            appendHistogram(bob, collClonerFillBatchForInsertLatencyMillis);
            break;
        case Role::kCoordinator:
            bob->append(kCoordinatorState, serializeState(coordinatorState));
            bob->append(kOpStatus, ReshardingOperationStatus_serializer(opStatus));
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

ReshardingMetrics* ReshardingMetrics::get(ServiceContext* ctx) noexcept {
    return getMetrics(ctx).get();
}

void ReshardingMetrics::onStart(Role role, Date_t runningOperationStartTime) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    // TODO Re-add this invariant once all breaking test cases have been fixed.
    // invariant(!_currentOp.has_value(), kAnotherOperationInProgress);

    if (!_currentOp) {
        // Only incremement _started if this is the first time resharding metrics is being invoked
        // for this resharding operation, and we're not restoring the PrimaryOnlyService from disk.
        _started++;
    }

    // Create a new operation and record the time it started.
    _emplaceCurrentOpForRole(role, runningOperationStartTime);
}

void ReshardingMetrics::onCompletion(Role role,
                                     ReshardingOperationStatusEnum status,
                                     Date_t runningOperationEndTime) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    // TODO Re-add this invariant once all breaking test cases have been fixed. Add invariant that
    // role being completed is a role that is in progress.
    // invariant(_currentOp.has_value(), kNoOperationInProgress);

    // Reset the cumulative min and max remaining operation time to 0. Only coordinators
    // will need to report this information, so only reset it for coordinators.
    if (role == ReshardingMetrics::Role::kCoordinator) {
        _cumulativeOp->minRemainingOperationTime = Milliseconds(0);
        _cumulativeOp->maxRemainingOperationTime = Milliseconds(0);
    }

    if (_currentOp->donorState && _currentOp->recipientState) {
        switch (role) {
            case Role::kDonor:
                _currentOp->donorState = boost::none;
                break;
            case Role::kRecipient:
                _currentOp->recipientState = boost::none;
                break;
            default:
                MONGO_UNREACHABLE;
        }

        return;
    }

    switch (status) {
        case ReshardingOperationStatusEnum::kSuccess:
            _succeeded++;
            break;
        case ReshardingOperationStatusEnum::kFailure:
            _failed++;
            break;
        case ReshardingOperationStatusEnum::kCanceled:
            _canceled++;
            break;
        default:
            MONGO_UNREACHABLE;
    }

    _currentOp->runningOperation.end(runningOperationEndTime);

    // Reset current op metrics.
    _currentOp = nullptr;
}

void ReshardingMetrics::onStepUp(Role role) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    _emplaceCurrentOpForRole(role, boost::none);

    // TODO SERVER-53914 Implement coordinator metrics rehydration.

    // TODO SERVER-57094 Resume the runningOperation duration from a timestamp stored on disk
    // instead of starting from the current time.
}

void ReshardingMetrics::onStepUp(DonorStateEnum state, ReshardingDonorMetrics donorMetrics) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto operationRuntime = donorMetrics.getOperationRuntime();
    _emplaceCurrentOpForRole(
        Role::kDonor, operationRuntime.has_value() ? operationRuntime->getStart() : boost::none);
    _currentOp->donorState = state;

    if (auto criticalSectionTimeInterval = donorMetrics.getCriticalSection();
        criticalSectionTimeInterval.has_value() &&
        criticalSectionTimeInterval->getStart().has_value()) {
        _currentOp->inCriticalSection.start(criticalSectionTimeInterval->getStart().get());

        if (auto stopTime = criticalSectionTimeInterval->getStop(); stopTime.has_value())
            _currentOp->inCriticalSection.forceEnd(stopTime.get());
    }
}

void ReshardingMetrics::onStepDown(Role role) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_currentOp && _currentOp->donorState && _currentOp->recipientState) {
        switch (role) {
            case Role::kDonor:
                _currentOp->donorState = boost::none;
                break;
            case Role::kRecipient:
                _currentOp->recipientState = boost::none;
                break;
            default:
                MONGO_UNREACHABLE;
        }
    } else {
        _currentOp = nullptr;
    }
}

void ReshardingMetrics::_emplaceCurrentOpForRole(
    Role role, boost::optional<Date_t> runningOperationStartTime) noexcept {
    // Invariants in this function ensure that the only multi-role state allowed is a combination
    // of donor and recipient.
    if (!_currentOp) {
        _currentOp = std::make_unique<OperationMetrics>();
        _currentOp->runningOperation.start(runningOperationStartTime
                                               ? *runningOperationStartTime
                                               : _svcCtx->getFastClockSource()->now());
        _currentOp->opStatus = ReshardingOperationStatusEnum::kRunning;
    } else {
        invariant(role != Role::kCoordinator, kAnotherOperationInProgress);
        invariant(!_currentOp->coordinatorState, kAnotherOperationInProgress);
    }

    switch (role) {
        case Role::kCoordinator:
            _currentOp->coordinatorState.emplace(CoordinatorStateEnum::kUnused);
            break;
        case Role::kDonor:
            invariant(!_currentOp->donorState, kAnotherOperationInProgress);
            _currentOp->donorState.emplace(DonorStateEnum::kUnused);
            break;
        case Role::kRecipient:
            invariant(!_currentOp->recipientState, kAnotherOperationInProgress);
            _currentOp->recipientState.emplace(RecipientStateEnum::kUnused);
            break;
        default:
            MONGO_UNREACHABLE
    }
}

void ReshardingMetrics::setDonorState(DonorStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp, kNoOperationInProgress);

    const auto oldState = std::exchange(_currentOp->donorState, state);
    invariant(oldState != state);
}

void ReshardingMetrics::setRecipientState(RecipientStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp, kNoOperationInProgress);

    const auto oldState = std::exchange(_currentOp->recipientState, state);
    invariant(oldState != state);
}

void ReshardingMetrics::setCoordinatorState(CoordinatorStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp, kNoOperationInProgress);
    _currentOp->coordinatorState = state;
}

template <typename T>
static bool checkState(T state, std::initializer_list<T> validStates) {
    invariant(validStates.size());
    if (std::find(validStates.begin(), validStates.end(), state) != validStates.end())
        return true;

    std::stringstream ss;
    StringData sep = "";
    for (auto s : validStates) {
        ss << sep << serializeState(s);
        sep = ", "_sd;
    }

    LOGV2_FATAL_CONTINUE(5553300,
                         "Invalid resharding state",
                         "state"_attr = serializeState(state),
                         "valid"_attr = ss.str());
    return false;
}

void ReshardingMetrics::setDocumentsToCopy(int64_t documents, int64_t bytes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp, kNoOperationInProgress);
    invariant(_currentOp->recipientState == RecipientStateEnum::kCreatingCollection);

    setDocumentsToCopyForCurrentOp(documents, bytes);
}

void ReshardingMetrics::setDocumentsToCopyForCurrentOp(int64_t documents, int64_t bytes) noexcept {
    invariant(_currentOp, kNoOperationInProgress);

    _currentOp->documentsToCopy = documents;
    _currentOp->bytesToCopy = bytes;
}

void ReshardingMetrics::setLastReshardChunkImbalanceCount(int64_t newCount) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);

    invariant(_currentOp, kNoOperationInProgress);
    invariant(_currentOp->coordinatorState);

    _cumulativeOp->chunkImbalanceCount = newCount;
}

void ReshardingMetrics::setMinRemainingOperationTime(Milliseconds minTime) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_currentOp) {
        _cumulativeOp->minRemainingOperationTime = minTime;
    }
}

void ReshardingMetrics::setMaxRemainingOperationTime(Milliseconds maxTime) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_currentOp) {
        _cumulativeOp->maxRemainingOperationTime = maxTime;
    }
}

void ReshardingMetrics::onDocumentsCopied(int64_t documents, int64_t bytes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return;

    invariant(checkState(*_currentOp->recipientState,
                         {RecipientStateEnum::kCloning, RecipientStateEnum::kError}));

    _currentOp->documentsCopied += documents;
    _currentOp->bytesCopied += bytes;
    _cumulativeOp->documentsCopied += documents;
    _cumulativeOp->bytesCopied += bytes;
}

void ReshardingMetrics::gotInserts(int n) noexcept {
    _cumulativeOp->gotInserts(n);
}

void ReshardingMetrics::onOplogApplierApplyBatch(Milliseconds latency) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp, kNoOperationInProgress);
    invariant(checkState(*_currentOp->recipientState,
                         {RecipientStateEnum::kApplying, RecipientStateEnum::kError}));

    _currentOp->oplogApplierApplyBatchLatencyMillis.increment(durationCount<Milliseconds>(latency));
    _cumulativeOp->oplogApplierApplyBatchLatencyMillis.increment(
        durationCount<Milliseconds>(latency));
}

void ReshardingMetrics::onCollClonerFillBatchForInsert(Milliseconds latency) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp, kNoOperationInProgress);
    invariant(checkState(*_currentOp->recipientState,
                         {RecipientStateEnum::kCloning, RecipientStateEnum::kError}));

    _currentOp->collClonerFillBatchForInsertLatencyMillis.increment(
        durationCount<Milliseconds>(latency));
    _cumulativeOp->collClonerFillBatchForInsertLatencyMillis.increment(
        durationCount<Milliseconds>(latency));
}

void ReshardingMetrics::gotInsert() noexcept {
    _cumulativeOp->gotInsert();
}

void ReshardingMetrics::gotUpdate() noexcept {
    _cumulativeOp->gotUpdate();
}

void ReshardingMetrics::gotDelete() noexcept {
    _cumulativeOp->gotDelete();
}

void ReshardingMetrics::startCopyingDocuments(Date_t start) {
    stdx::lock_guard<Latch> lk(_mutex);
    _currentOp->copyingDocuments.start(start);
}

void ReshardingMetrics::endCopyingDocuments(Date_t end) {
    stdx::lock_guard<Latch> lk(_mutex);
    _currentOp->copyingDocuments.forceEnd(end);
}

void ReshardingMetrics::startApplyingOplogEntries(Date_t start) {
    stdx::lock_guard<Latch> lk(_mutex);
    _currentOp->applyingOplogEntries.start(start);
}

void ReshardingMetrics::endApplyingOplogEntries(Date_t end) {
    stdx::lock_guard<Latch> lk(_mutex);
    _currentOp->applyingOplogEntries.forceEnd(end);
}

void ReshardingMetrics::enterCriticalSection(Date_t start) {
    stdx::lock_guard<Latch> lk(_mutex);
    _currentOp->inCriticalSection.start(start);
}

void ReshardingMetrics::leaveCriticalSection(Date_t end) {
    stdx::lock_guard<Latch> lk(_mutex);
    _currentOp->inCriticalSection.forceEnd(end);
}

void ReshardingMetrics::onOplogEntriesFetched(int64_t entries) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return;

    invariant(checkState(
        *_currentOp->recipientState,
        {RecipientStateEnum::kCloning, RecipientStateEnum::kApplying, RecipientStateEnum::kError}));

    _currentOp->oplogEntriesFetched += entries;
    _cumulativeOp->oplogEntriesFetched += entries;
}

void ReshardingMetrics::onOplogEntriesApplied(int64_t entries) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return;

    invariant(checkState(*_currentOp->recipientState,
                         {RecipientStateEnum::kApplying, RecipientStateEnum::kError}));

    _currentOp->oplogEntriesApplied += entries;
    _cumulativeOp->oplogEntriesApplied += entries;
}

void ReshardingMetrics::restoreForCurrentOp(int64_t documentCountCopied,
                                            int64_t documentBytesCopied,
                                            int64_t oplogEntriesFetched,
                                            int64_t oplogEntriesApplied) noexcept {
    invariant(_currentOp, kNoOperationInProgress);
    invariant(_currentOp->documentsCopied == 0, kMetricsSetBeforeRestore);
    invariant(_currentOp->bytesCopied == 0, kMetricsSetBeforeRestore);
    invariant(_currentOp->oplogEntriesFetched == 0, kMetricsSetBeforeRestore);
    invariant(_currentOp->oplogEntriesApplied == 0, kMetricsSetBeforeRestore);

    _currentOp->documentsCopied = documentCountCopied;
    _currentOp->bytesCopied = documentBytesCopied;
    _currentOp->oplogEntriesFetched = oplogEntriesFetched;
    _currentOp->oplogEntriesApplied = oplogEntriesApplied;
}

void ReshardingMetrics::onWriteDuringCriticalSection(int64_t writes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return;

    invariant(checkState(*_currentOp->donorState,
                         {DonorStateEnum::kPreparingToBlockWrites,
                          DonorStateEnum::kBlockingWrites,
                          DonorStateEnum::kError}));

    onWriteDuringCriticalSectionForCurrentOp(writes);
    _cumulativeOp->writesDuringCriticalSection += writes;
}

void ReshardingMetrics::onWriteDuringCriticalSectionForCurrentOp(int64_t writes) noexcept {
    invariant(_currentOp, kNoOperationInProgress);

    _currentOp->writesDuringCriticalSection += writes;
}

void ReshardingMetrics::serializeCurrentOpMetrics(BSONObjBuilder* bob, Role role) const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_currentOp)
        _currentOp->appendCurrentOpMetrics(bob, role, _now());
}

BSONObj ReshardingMetrics::reportForCurrentOp(const ReporterOptions& options) const noexcept {
    auto roleName = [](Role role) {
        switch (role) {
            case Role::kDonor:
                return "Donor"_sd;
            case Role::kRecipient:
                return "Recipient"_sd;
            case Role::kCoordinator:
                return "Coordinator"_sd;
        }
        MONGO_UNREACHABLE;
    };

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append(
        "desc",
        fmt::format("Resharding{}Service {}", roleName(options.role), options.id.toString()));
    bob.append("op", "command");
    bob.append("ns", options.nss.toString());

    {
        BSONObjBuilder originating{bob.subobjStart("originatingCommand")};
        originating.append("reshardCollection", options.nss.toString());
        originating.append("key", options.shardKey);
        originating.append("unique", options.unique);
        BSONObjBuilder{originating.subobjStart("collation")}.append("locale", "simple");
    }

    serializeCurrentOpMetrics(&bob, options.role);

    return bob.obj();
}

boost::optional<Milliseconds> ReshardingMetrics::getOperationElapsedTime() const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return boost::none;
    return _currentOp->runningOperation.duration(_now());
}

boost::optional<Milliseconds> ReshardingMetrics::getOperationRemainingTime() const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_currentOp)
        return _currentOp->remainingOperationTime(_now());
    return boost::none;
}

void ReshardingMetrics::serializeCumulativeOpMetrics(BSONObjBuilder* bob) const {
    stdx::lock_guard<Latch> lk(_mutex);

    auto getRemainingOperationTime = [&](const Milliseconds& time) -> int64_t {
        return durationCount<Milliseconds>(time);
    };

    bob->append(kTotalOps, _started);
    bob->append(kSuccessfulOps, _succeeded);
    bob->append(kFailedOps, _failed);
    bob->append(kCanceledOps, _canceled);

    const auto& ops = *_cumulativeOp;
    bob->append(kDocumentsCopied, ops.documentsCopied);
    bob->append(kBytesCopied, ops.bytesCopied);
    bob->append(kOplogsApplied, ops.oplogEntriesApplied);
    bob->append(kWritesDuringCritialSection, ops.writesDuringCriticalSection);
    bob->append(kOplogsFetched, ops.oplogEntriesFetched);
    bob->append(kLastOpEndingChunkImbalance, ops.chunkImbalanceCount);
    bob->append(kOpCounters, ops.opCounters.getObj());
    bob->append(kMinRemainingOperationTime,
                getRemainingOperationTime(ops.minRemainingOperationTime));
    bob->append(kMaxRemainingOperationTime,
                getRemainingOperationTime(ops.maxRemainingOperationTime));

    appendHistogram(bob, ops.oplogApplierApplyBatchLatencyMillis);
    appendHistogram(bob, ops.collClonerFillBatchForInsertLatencyMillis);
}

Date_t ReshardingMetrics::_now() const {
    return _svcCtx->getFastClockSource()->now();
}

ReshardingMetrics::ReshardingMetrics(ServiceContext* svcCtx)
    : _svcCtx{svcCtx}, _cumulativeOp{std::make_unique<OperationMetrics>()} {}

ReshardingMetrics::~ReshardingMetrics() = default;

}  // namespace mongo
