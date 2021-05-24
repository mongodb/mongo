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
    _currentOp = boost::none;
}

void ReshardingMetrics::onStepUp(Role role) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    _emplaceCurrentOpForRole(role, boost::none);

    // TODO SERVER-53913 Implement donor metrics rehydration.
    // TODO SERVER-53914 Implement coordinator metrics rehydration.
    // TODO SERVER-53912 Implement recipient metrics rehydration.

    // TODO SERVER-57094 Resume the runningOperation duration from a timestamp stored on disk
    // instead of starting from the current time.
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
        _currentOp = boost::none;
    }
}

void ReshardingMetrics::_emplaceCurrentOpForRole(
    Role role, boost::optional<Date_t> runningOperationStartTime) noexcept {
    // Invariants in this function ensure that the only multi-role state allowed is a combination
    // of donor and recipient.
    if (!_currentOp) {
        _currentOp.emplace(_svcCtx->getFastClockSource());
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
    invariant(_currentOp.has_value(), kNoOperationInProgress);

    const auto oldState = std::exchange(_currentOp->donorState, state);
    invariant(oldState != state);
}

void ReshardingMetrics::setRecipientState(RecipientStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value(), kNoOperationInProgress);

    const auto oldState = std::exchange(_currentOp->recipientState, state);
    invariant(oldState != state);
}

void ReshardingMetrics::setCoordinatorState(CoordinatorStateEnum state) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_currentOp.has_value(), kNoOperationInProgress);
    _currentOp->coordinatorState = state;
}

// TODO SERVER-57217 Remove special-casing for the non-existence of the boost::optional.
static StringData serializeState(boost::optional<RecipientStateEnum> e) {
    return e ? RecipientState_serializer(*e)
             : RecipientState_serializer(RecipientStateEnum::kUnused);
}

// TODO SERVER-57217 Remove special-casing for the non-existence of the boost::optional.
static StringData serializeState(boost::optional<DonorStateEnum> e) {
    return e ? DonorState_serializer(*e) : DonorState_serializer(DonorStateEnum::kUnused);
}

// TODO SERVER-57217 Remove special-casing for the non-existence of the boost::optional.
static StringData serializeState(boost::optional<CoordinatorStateEnum> e) {
    return e ? CoordinatorState_serializer(*e)
             : CoordinatorState_serializer(CoordinatorStateEnum::kUnused);
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
    invariant(_currentOp->recipientState == RecipientStateEnum::kCreatingCollection);

    _currentOp->documentsToCopy = documents;
    _currentOp->bytesToCopy = bytes;
}

void ReshardingMetrics::onDocumentsCopied(int64_t documents, int64_t bytes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return;

    invariant(checkState(*_currentOp->recipientState,
                         {RecipientStateEnum::kCloning, RecipientStateEnum::kError}));

    _currentOp->documentsCopied += documents;
    _currentOp->bytesCopied += bytes;
    _cumulativeOp.documentsCopied += documents;
    _cumulativeOp.bytesCopied += bytes;
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

void ReshardingMetrics::startInCriticalSection(Date_t start) {
    stdx::lock_guard<Latch> lk(_mutex);
    _currentOp->inCriticalSection.start(start);
}

void ReshardingMetrics::endInCriticalSection(Date_t end) {
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
    _cumulativeOp.oplogEntriesFetched += entries;
}

void ReshardingMetrics::onOplogEntriesApplied(int64_t entries) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return;

    invariant(checkState(*_currentOp->recipientState,
                         {RecipientStateEnum::kApplying, RecipientStateEnum::kError}));

    _currentOp->oplogEntriesApplied += entries;
    _cumulativeOp.oplogEntriesApplied += entries;
}

void ReshardingMetrics::onWriteDuringCriticalSection(int64_t writes) noexcept {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_currentOp)
        return;

    invariant(checkState(*_currentOp->donorState,
                         {DonorStateEnum::kDonatingOplogEntries,
                          DonorStateEnum::kBlockingWrites,
                          DonorStateEnum::kError}));

    _currentOp->writesDuringCriticalSection += writes;
    _cumulativeOp.writesDuringCriticalSection += writes;
}

void ReshardingMetrics::OperationMetrics::TimeInterval::start(Date_t start) noexcept {
    invariant(!_start.has_value(), "Already started");
    _start.emplace(start);
}

void ReshardingMetrics::OperationMetrics::TimeInterval::end(Date_t end) noexcept {
    invariant(_start.has_value(), "Not started");
    invariant(!_end.has_value(), "Already stopped");
    _end.emplace(end);
}

void ReshardingMetrics::OperationMetrics::TimeInterval::forceEnd(Date_t end) noexcept {
    if (!_start.has_value()) {
        _start.emplace(end);
    }

    this->end(end);
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


    const auto remainingMsec = remainingOperationTime();

    bob->append(kOpTimeElapsed, getElapsedTime(runningOperation));

    bob->append(kOpTimeRemaining,
                !remainingMsec ? int64_t{-1} /** -1 is a specified integer null value */
                               : durationCount<Seconds>(*remainingMsec));

    switch (role) {
        case Role::kDonor:
            bob->append(kWritesDuringCritialSection, writesDuringCriticalSection);
            bob->append(kCriticalSectionTimeElapsed, getElapsedTime(inCriticalSection));
            bob->append(kDonorState, serializeState(donorState));
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
            bob->append(kRecipientState, serializeState(recipientState));
            bob->append(kOpStatus, ReshardingOperationStatus_serializer(opStatus));
            break;
        case Role::kCoordinator:
            bob->append(kCoordinatorState, serializeState(coordinatorState));
            bob->append(kOpStatus, ReshardingOperationStatus_serializer(opStatus));
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void ReshardingMetrics::serializeCurrentOpMetrics(BSONObjBuilder* bob, Role role) const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_currentOp)
        _currentOp->appendCurrentOpMetrics(bob, role);
}

BSONObj ReshardingMetrics::reportForCurrentOp(const ReporterOptions& options) const noexcept {
    const auto role = [&options] {
        switch (options.role) {
            case Role::kDonor:
                return "Donor";
            case Role::kRecipient:
                return "Recipient";
            case Role::kCoordinator:
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

boost::optional<Milliseconds> ReshardingMetrics::getOperationRemainingTime() const {
    if (_currentOp)
        return _currentOp->remainingOperationTime();
    return boost::none;
}

void ReshardingMetrics::OperationMetrics::appendCumulativeOpMetrics(BSONObjBuilder* bob) const {
    bob->append(kDocumentsCopied, documentsCopied);
    bob->append(kBytesCopied, bytesCopied);
    bob->append(kOplogsApplied, oplogEntriesApplied);
    bob->append(kWritesDuringCritialSection, writesDuringCriticalSection);
    bob->append(kOplogsFetched, oplogEntriesFetched);
}

boost::optional<Milliseconds> ReshardingMetrics::OperationMetrics::remainingOperationTime() const {
    if (recipientState > RecipientStateEnum::kCloning && oplogEntriesFetched == 0) {
        return Milliseconds(0);
    }

    if (oplogEntriesApplied > 0 && oplogEntriesFetched > 0) {
        // All fetched oplogEntries must be applied. Some of them already have been.
        return remainingTime(
            applyingOplogEntries.duration(), oplogEntriesApplied, oplogEntriesFetched);
    }
    if (bytesCopied > 0 && bytesToCopy > 0) {
        // Until the time to apply batches of oplog entries is measured, we assume that applying all
        // of them will take as long as copying did.
        return remainingTime(copyingDocuments.duration(), bytesCopied, 2 * bytesToCopy);
    }
    return {};
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
