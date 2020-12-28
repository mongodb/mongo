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

#include <algorithm>
#include <memory>

#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/platform/compiler.h"

namespace mongo {

namespace {
constexpr auto kAnotherOperationInProgress = "Another operation is in progress";
constexpr auto kNoOperationInProgress = "No operation is in progress";

constexpr auto kSuccessfulOps = "successfulOperations";
constexpr auto kFailedOps = "failedOperations";
constexpr auto kCanceledOps = "canceledOperations";
constexpr auto kOpTimeElapsed = "totalOperationTimeElapsedMillis";
constexpr auto kOpTimeRemaining = "remainingOperationTimeEstimatedMillis";
constexpr auto kDocumentsToCopy = "approxDocumentsToCopy";
constexpr auto kDocumentsCopied = "documentsCopied";
constexpr auto kBytesToCopy = "approxBytesToCopy";
constexpr auto kBytesCopied = "bytesCopied";
constexpr auto kCopyTimeElapsed = "totalCopyTimeElapsedMillis";
constexpr auto kOplogsFetched = "oplogEntriesFetched";
constexpr auto kOplogsApplied = "oplogEntriesApplied";
constexpr auto kApplyTimeElapsed = "totalApplyTimeElapsedMillis";
constexpr auto kWritesDuringCritialSection = "countWritesDuringCriticalSection";
constexpr auto kCriticalSectionTimeElapsed = "totalCriticalSectionTimeElapsedMillis";
constexpr auto kCoordinatorState = "coordinatorState";
constexpr auto kDonorState = "donorState";
constexpr auto kRecipientState = "recipientState";
constexpr auto kCompletionStatus = "opStatus";

using MetricsPtr = std::unique_ptr<ReshardingMetrics>;

const auto getMetrics = ServiceContext::declareDecoration<MetricsPtr>();

const auto reshardingMetricsRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ReshardingMetrics",
    [](ServiceContext* ctx) { getMetrics(ctx) = std::make_unique<ReshardingMetrics>(ctx); }};
}  // namespace

ReshardingMetrics* ReshardingMetrics::get(ServiceContext* ctx) noexcept {
    return getMetrics(ctx).get();
}

void ReshardingMetrics::onStart() noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_currentOp.has_value() || _currentOp->isCompleted(), kAnotherOperationInProgress);

    // Create a new operation and record the time it started.
    _currentOp.emplace(_svcCtx->getFastClockSource());
    _currentOp->runningOperation.start();
}

void ReshardingMetrics::onCompletion(ReshardingMetrics::OperationStatus status) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);
    switch (status) {
        case OperationStatus::kSucceeded:
            _succeeded++;
            break;
        case OperationStatus::kFailed:
            _failed++;
            break;
        case OperationStatus::kCanceled:
            _canceled++;
            break;
        default:
            MONGO_UNREACHABLE;
    }

    // Mark the active operation as completed and ensure all timers are stopped.
    _currentOp->runningOperation.end();
    _currentOp->copyingDocuments.tryEnd();
    _currentOp->applyingOplogEntries.tryEnd();
    _currentOp->inCriticalSection.tryEnd();
    _currentOp->completionStatus.emplace(std::move(status));
}

void ReshardingMetrics::setDonorState(DonorStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);

    const auto oldState = std::exchange(_currentOp->donorState, state);
    invariant(oldState != state);

    if (state == DonorStateEnum::kPreparingToMirror) {
        _currentOp->inCriticalSection.start();
    }

    if (oldState == DonorStateEnum::kMirroring) {
        _currentOp->inCriticalSection.end();
    }
}

void ReshardingMetrics::setRecipientState(RecipientStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);

    const auto oldState = std::exchange(_currentOp->recipientState, state);
    invariant(oldState != state);

    if (state == RecipientStateEnum::kCloning) {
        _currentOp->copyingDocuments.start();
    } else if (state == RecipientStateEnum::kApplying) {
        _currentOp->applyingOplogEntries.start();
    }

    if (oldState == RecipientStateEnum::kCloning) {
        _currentOp->copyingDocuments.end();
    } else if (oldState == RecipientStateEnum::kApplying) {
        _currentOp->applyingOplogEntries.end();
    }
}

void ReshardingMetrics::setCoordinatorState(CoordinatorStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);
    _currentOp->coordinatorState = state;
}

void ReshardingMetrics::setDocumentsToCopy(int64_t documents, int64_t bytes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);
    invariant(_currentOp->recipientState == RecipientStateEnum::kCloning);

    _currentOp->documentsToCopy = documents;
    _currentOp->bytesToCopy = bytes;
}

void ReshardingMetrics::onDocumentsCopied(int64_t documents, int64_t bytes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);
    invariant(_currentOp->recipientState == RecipientStateEnum::kCloning);

    _currentOp->documentsCopied += documents;
    _currentOp->bytesCopied += bytes;
}

void ReshardingMetrics::onOplogEntriesFetched(int64_t entries) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);
    invariant(_currentOp->recipientState == RecipientStateEnum::kCloning ||
              _currentOp->recipientState == RecipientStateEnum::kApplying ||
              _currentOp->recipientState == RecipientStateEnum::kSteadyState);
    _currentOp->oplogEntriesFetched += entries;
}

void ReshardingMetrics::onOplogEntriesApplied(int64_t entries) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);
    invariant(_currentOp->recipientState == RecipientStateEnum::kApplying ||
              _currentOp->recipientState == RecipientStateEnum::kSteadyState);
    _currentOp->oplogEntriesApplied += entries;
}

void ReshardingMetrics::onWriteDuringCriticalSection(int64_t writes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value() && !_currentOp->isCompleted(), kNoOperationInProgress);
    invariant(_currentOp->donorState == DonorStateEnum::kPreparingToMirror ||
              _currentOp->donorState == DonorStateEnum::kMirroring);
    _currentOp->writesDuringCriticalSection += writes;
}

void ReshardingMetrics::OperationMetrics::TimeInterval::start() noexcept {
    invariant(!_start.has_value(), "Already started");
    _start.emplace(_clockSource->now());
}

void ReshardingMetrics::OperationMetrics::TimeInterval::tryEnd() noexcept {
    if (!_start.has_value())
        return;
    if (_end.has_value())
        return;
    _end.emplace(_clockSource->now());
}

void ReshardingMetrics::OperationMetrics::TimeInterval::end() noexcept {
    invariant(_start.has_value(), "Not started");
    invariant(!_end.has_value(), "Already stopped");
    tryEnd();
}

Milliseconds ReshardingMetrics::OperationMetrics::TimeInterval::duration() const noexcept {
    if (!_start.has_value())
        return Milliseconds(0);
    if (!_end.has_value())
        return duration_cast<Milliseconds>(_clockSource->now() - _start.value());
    return duration_cast<Milliseconds>(_end.value() - _start.value());
}

void ReshardingMetrics::OperationMetrics::append(BSONObjBuilder* bob) const {
    auto getElapsedTime = [](const TimeInterval& interval) -> int64_t {
        return durationCount<Milliseconds>(interval.duration());
    };

    auto estimateRemainingOperationTime = [&]() -> int64_t {
        if (bytesCopied == 0 && oplogEntriesApplied == 0)
            return -1;
        else if (oplogEntriesApplied == 0) {
            invariant(bytesCopied > 0);
            // Until the time to apply batches of oplog entries is measured, we assume that applying
            // all of them will take as long as copying did.
            const auto elapsedCopyTime = getElapsedTime(copyingDocuments);
            const auto approxTimeToCopy =
                elapsedCopyTime * std::max((int64_t)0, bytesToCopy / bytesCopied - 1);
            return elapsedCopyTime + 2 * approxTimeToCopy;
        } else {
            invariant(oplogEntriesApplied > 0);
            const auto approxTimeToApply = getElapsedTime(applyingOplogEntries) *
                std::max((int64_t)0, oplogEntriesFetched / oplogEntriesApplied - 1);
            return approxTimeToApply;
        }
    };

    bob->append(kOpTimeElapsed, getElapsedTime(runningOperation));
    bob->append(kOpTimeRemaining, estimateRemainingOperationTime());

    bob->append(kDocumentsToCopy, documentsToCopy);
    bob->append(kDocumentsCopied, documentsCopied);
    bob->append(kBytesToCopy, bytesToCopy);
    bob->append(kBytesCopied, bytesCopied);
    bob->append(kCopyTimeElapsed, getElapsedTime(copyingDocuments));

    bob->append(kOplogsFetched, oplogEntriesFetched);
    bob->append(kOplogsApplied, oplogEntriesApplied);
    bob->append(kApplyTimeElapsed, getElapsedTime(applyingOplogEntries));

    bob->append(kWritesDuringCritialSection, writesDuringCriticalSection);
    bob->append(kCriticalSectionTimeElapsed, getElapsedTime(inCriticalSection));

    bob->append(kDonorState, donorState);
    bob->append(kRecipientState, recipientState);
    bob->append(kCoordinatorState, coordinatorState);

    bob->append(kCompletionStatus, completionStatus.value_or(OperationStatus::kUnknown));
}

void ReshardingMetrics::serialize(BSONObjBuilder* bob) const {
    stdx::lock_guard<Latch> lk(_mutex);

    bob->append(kSuccessfulOps, _succeeded);
    bob->append(kFailedOps, _failed);
    bob->append(kCanceledOps, _canceled);

    if (_currentOp) {
        _currentOp->append(bob);
    } else {
        // There are no resharding operations in progress, so report the default metrics.
        OperationMetrics opMetrics(_svcCtx->getFastClockSource());
        opMetrics.append(bob);
    }
}

}  // namespace mongo
