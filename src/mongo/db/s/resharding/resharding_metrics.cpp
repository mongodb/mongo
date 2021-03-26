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

namespace mongo {

namespace {
constexpr auto kAnotherOperationInProgress = "Another operation is in progress";
constexpr auto kNoOperationInProgress = "No operation is in progress";

constexpr auto kTotalOps = "countReshardingOperations";
constexpr auto kSuccessfulOps = "countReshardingSuccessful";
constexpr auto kFailedOps = "countReshardingFailures";
constexpr auto kCanceledOps = "countReshardingCanceled";
constexpr auto kOpTimeElapsed = "totalOperationTimeElapsed";
constexpr auto kOpTimeRemaining = "remainingOperationTimeEstimated";
constexpr auto kDocumentsToCopy = "approxDocumentsToCopy";
constexpr auto kDocumentsCopied = "documentsCopied";
constexpr auto kBytesToCopy = "approxBytesToCopy";
constexpr auto kBytesCopied = "bytesCopied";
constexpr auto kCopyTimeElapsed = "totalCopyTimeElapsed";
constexpr auto kOplogsFetched = "oplogEntriesFetched";
constexpr auto kOplogsApplied = "oplogEntriesApplied";
constexpr auto kApplyTimeElapsed = "totalApplyTimeElapsed";
constexpr auto kWritesDuringCritialSection = "countWritesDuringCriticalSection";
constexpr auto kCriticalSectionTimeElapsed = "totalCriticalSectionTimeElapsed";
constexpr auto kCoordinatorState = "coordinatorState";
constexpr auto kDonorState = "donorState";
constexpr auto kRecipientState = "recipientState";
constexpr auto kOpStatus = "opStatus";

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
}  // namespace

ReshardingMetrics* ReshardingMetrics::get(ServiceContext* ctx) noexcept {
    return getMetrics(ctx).get();
}

void ReshardingMetrics::onStart() noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_currentOp.has_value(), kAnotherOperationInProgress);
    // Create a new operation and record the time it started.
    _currentOp.emplace(_svcCtx->getFastClockSource());
    _currentOp->runningOperation.start();
    _currentOp->opStatus = ReshardingOperationStatusEnum::kRunning;
    _started++;
}

void ReshardingMetrics::onCompletion(ReshardingOperationStatusEnum status) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value(), kNoOperationInProgress);
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

    // Reset current op metrics.
    _currentOp = boost::none;
}

void ReshardingMetrics::setDonorState(DonorStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value(), kNoOperationInProgress);

    const auto oldState = std::exchange(_currentOp->donorState, state);
    invariant(oldState != state);

    if (state == DonorStateEnum::kPreparingToBlockWrites) {
        _currentOp->inCriticalSection.start();
    }

    if (oldState == DonorStateEnum::kBlockingWrites) {
        _currentOp->inCriticalSection.end();
    }
}

void ReshardingMetrics::setRecipientState(RecipientStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value(), kNoOperationInProgress);

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
    invariant(_currentOp.has_value(), kNoOperationInProgress);
    _currentOp->coordinatorState = state;
}

static StringData serializeState(RecipientStateEnum e) {
    return RecipientState_serializer(e);
}

static StringData serializeState(DonorStateEnum e) {
    return DonorState_serializer(e);
}

template <typename T>
static bool checkState(T state, std::initializer_list<T> validStates) {
    invariant(validStates.size());
    if (std::find(validStates.begin(), validStates.end(), state) != validStates.end())
        return true;

    std::stringstream ss;
    StringData sep = "";
    for (auto state : validStates) {
        ss << sep << serializeState(state);
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
    invariant(_currentOp.has_value(), kNoOperationInProgress);

    _currentOp->documentsToCopy = documents;
    _currentOp->bytesToCopy = bytes;
}

void ReshardingMetrics::onDocumentsCopied(int64_t documents, int64_t bytes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);

    invariant(_currentOp &&
              checkState(_currentOp->recipientState,
                         {RecipientStateEnum::kCloning, RecipientStateEnum::kError}));

    _currentOp->documentsCopied += documents;
    _currentOp->bytesCopied += bytes;
    _cumulativeOp.documentsCopied += documents;
    _cumulativeOp.bytesCopied += bytes;
}

void ReshardingMetrics::onOplogEntriesFetched(int64_t entries) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);

    invariant(_currentOp &&
              checkState(_currentOp->recipientState,
                         {RecipientStateEnum::kCloning,
                          RecipientStateEnum::kApplying,
                          RecipientStateEnum::kSteadyState,
                          RecipientStateEnum::kError}));

    _currentOp->oplogEntriesFetched += entries;
    _cumulativeOp.oplogEntriesFetched += entries;
}

void ReshardingMetrics::onOplogEntriesApplied(int64_t entries) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);

    invariant(_currentOp &&
              checkState(_currentOp->recipientState,
                         {RecipientStateEnum::kApplying,
                          RecipientStateEnum::kSteadyState,
                          RecipientStateEnum::kError}));

    _currentOp->oplogEntriesApplied += entries;
    _cumulativeOp.oplogEntriesApplied += entries;
}

void ReshardingMetrics::onWriteDuringCriticalSection(int64_t writes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);

    invariant(_currentOp.has_value(), kNoOperationInProgress);

    invariant(_currentOp &&
              checkState(_currentOp->donorState,
                         {DonorStateEnum::kDonatingOplogEntries,
                          DonorStateEnum::kPreparingToBlockWrites,
                          DonorStateEnum::kBlockingWrites,
                          DonorStateEnum::kError}));

    _currentOp->writesDuringCriticalSection += writes;
    _cumulativeOp.writesDuringCriticalSection += writes;
}

void ReshardingMetrics::OperationMetrics::TimeInterval::start() noexcept {
    invariant(!_start.has_value(), "Already started");
    _start.emplace(_clockSource->now());
}

void ReshardingMetrics::OperationMetrics::TimeInterval::end() noexcept {
    invariant(_start.has_value(), "Not started");
    invariant(!_end.has_value(), "Already stopped");
    _end.emplace(_clockSource->now());
}

Milliseconds ReshardingMetrics::OperationMetrics::TimeInterval::duration() const noexcept {
    if (!_start.has_value())
        return Milliseconds(0);
    if (!_end.has_value())
        return duration_cast<Milliseconds>(_clockSource->now() - _start.value());
    return duration_cast<Milliseconds>(_end.value() - _start.value());
}

void ReshardingMetrics::OperationMetrics::appendCurrentOpMetrics(BSONObjBuilder* bob,
                                                                 Role role) const {
    auto getElapsedTime = [](const TimeInterval& interval) -> int64_t {
        return durationCount<Seconds>(interval.duration());
    };

    auto remainingMsec = [&]() -> boost::optional<Milliseconds> {
        if (oplogEntriesApplied > 0 && oplogEntriesFetched > 0) {
            // All fetched oplogEntries must be applied. Some of them already have been.
            return remainingTime(
                applyingOplogEntries.duration(), oplogEntriesApplied, oplogEntriesFetched);
        }
        if (bytesCopied > 0 && bytesToCopy > 0) {
            // Until the time to apply batches of oplog entries is measured, we assume that applying
            // all of them will take as long as copying did.
            return remainingTime(copyingDocuments.duration(), bytesCopied, 2 * bytesToCopy);
        }
        return {};
    }();

    bob->append(kOpTimeElapsed, getElapsedTime(runningOperation));

    bob->append(kOpTimeRemaining,
                !remainingMsec ? int64_t{-1} /** -1 is a specified integer null value */
                               : durationCount<Seconds>(*remainingMsec));

    switch (role) {
        case Role::kDonor:
            bob->append(kWritesDuringCritialSection, writesDuringCriticalSection);
            bob->append(kCriticalSectionTimeElapsed, getElapsedTime(inCriticalSection));
            bob->append(kDonorState, DonorState_serializer(donorState));
            bob->append(kOpStatus, ReshardingOperationStatus_serializer(opStatus));
            break;
        case Role::kRecipient:
            bob->append(kDocumentsToCopy, documentsToCopy);
            bob->append(kDocumentsCopied, documentsCopied);
            bob->append(kBytesToCopy, bytesToCopy);
            bob->append(kBytesCopied, bytesCopied);
            bob->append(kCopyTimeElapsed, getElapsedTime(copyingDocuments));

            bob->append(kOplogsFetched, oplogEntriesFetched);
            bob->append(kOplogsApplied, oplogEntriesApplied);
            bob->append(kApplyTimeElapsed, getElapsedTime(applyingOplogEntries));
            bob->append(kRecipientState, RecipientState_serializer(recipientState));
            bob->append(kOpStatus, ReshardingOperationStatus_serializer(opStatus));
            break;
        case Role::kCoordinator:
            bob->append(kCoordinatorState, CoordinatorState_serializer(coordinatorState));
            bob->append(kOpStatus, ReshardingOperationStatus_serializer(opStatus));
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void ReshardingMetrics::serializeCurrentOpMetrics(BSONObjBuilder* bob,
                                                  ReporterOptions::Role role) const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_currentOp)
        _currentOp->appendCurrentOpMetrics(bob, role);
}

BSONObj ReshardingMetrics::reportForCurrentOp(const ReporterOptions& options) const noexcept {
    const auto role = [&options] {
        switch (options.role) {
            case ReporterOptions::Role::kDonor:
                return "Donor";
            case ReporterOptions::Role::kRecipient:
                return "Recipient";
            case ReporterOptions::Role::kCoordinator:
                return "Coordinator";
            default:
                MONGO_UNREACHABLE;
        }
    }();

    const auto originatingCommand =
        [&options] {
            BSONObjBuilder bob;
            bob.append("reshardCollection", options.nss.toString());
            bob.append("key", options.shardKey);
            bob.append("unique", options.unique);
            bob.append("collation",
                       BSON("locale"
                            << "simple"));
            return bob.obj();
        }();

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", fmt::format("Resharding{}Service {}", role, options.id.toString()));
    bob.append("op", "command");
    bob.append("ns", options.nss.toString());
    bob.append("originatingCommand", originatingCommand);

    serializeCurrentOpMetrics(&bob, options.role);

    return bob.obj();
}

boost::optional<Milliseconds> ReshardingMetrics::getOperationElapsedTime() const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return boost::none;
    return _currentOp->runningOperation.duration();
}

void ReshardingMetrics::OperationMetrics::appendCumulativeOpMetrics(BSONObjBuilder* bob) const {
    bob->append(kDocumentsCopied, documentsCopied);
    bob->append(kBytesCopied, bytesCopied);
    bob->append(kOplogsApplied, oplogEntriesApplied);
    bob->append(kWritesDuringCritialSection, writesDuringCriticalSection);
    bob->append(kOplogsFetched, oplogEntriesFetched);
}

void ReshardingMetrics::serializeCumulativeOpMetrics(BSONObjBuilder* bob) const {
    stdx::lock_guard<Latch> lk(_mutex);

    bob->append(kTotalOps, _started);
    bob->append(kSuccessfulOps, _succeeded);
    bob->append(kFailedOps, _failed);
    bob->append(kCanceledOps, _canceled);

    _cumulativeOp.appendCumulativeOpMetrics(bob);
}

}  // namespace mongo
