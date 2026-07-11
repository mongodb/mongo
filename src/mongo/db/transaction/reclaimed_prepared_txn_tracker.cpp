// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/transaction/reclaimed_prepared_txn_tracker.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {
const auto reclaimedPreparedTxnTracker =
    ServiceContext::declareDecoration<ReclaimedPreparedTxnTracker>();
}

ReclaimedPreparedTxnTracker::ReclaimedPreparedTxnTracker(TickSource* tickSource)
    : _recoveryTimer(tickSource) {}

ReclaimedPreparedTxnTracker* ReclaimedPreparedTxnTracker::get(ServiceContext* serviceContext) {
    return &reclaimedPreparedTxnTracker(serviceContext);
}

ReclaimedPreparedTxnTracker* ReclaimedPreparedTxnTracker::get(OperationContext* opCtx) {
    return ReclaimedPreparedTxnTracker::get(opCtx->getServiceContext());
}

SharedSemiFuture<void> ReclaimedPreparedTxnTracker::onAllReclaimedPreparedTxnsResolved() {
    std::lock_guard<std::mutex> lk(_mutex);
    invariant(_discoveryStarted,
              "Attempting to wait on all reclaimed prepared txn before discovery of reclaimed "
              "prepared txns has started");
    invariant(_discoveryComplete,
              "Attempting to wait on all reclaimed prepared txns before discovery of reclaimed "
              "prepared txns is complete");
    return _state->allPreparedTxnsResolved.getFuture();
}

void ReclaimedPreparedTxnTracker::trackPrepareExit(SharedSemiFuture<void> onExitPrepareFuture) {
    std::lock_guard<std::mutex> lk(_mutex);
    invariant(_discoveryStarted,
              "Attempting to track reclaimed prepared txn before discovery has started");
    invariant(!_discoveryComplete,
              "Attempting to track reclaimed prepared txn after discovery has completed");
    // Safe to inline because the continuation only decrements an atomic counter and emplaces a
    // one-time completion promise. So running it on the resolving thread should not deadlock.
    onExitPrepareFuture.unsafeToInlineFuture().getAsync([state = _state](Status s) {
        auto remainingUnresolved = state->unresolvedPreparedTxnsCount.subtractAndFetch(1);
        if (remainingUnresolved == 0) {
            state->allPreparedTxnsResolved.emplaceValue();
        }
    });

    _remainingToTrack -= 1;
}

void ReclaimedPreparedTxnTracker::beginDiscovery(long long expectedCount) {
    std::lock_guard<std::mutex> lk(_mutex);
    invariant(!_discoveryStarted,
              "Beginning discovery of reclaimed prepared txns after it has already started");
    invariant(!_discoveryComplete,
              "Beginning discovery of reclaimed prepared txns after it has already completed");
    _discoveryStarted = true;
    _state = std::make_shared<State>();
    _state->unresolvedPreparedTxnsCount.store(expectedCount);
    _remainingToTrack = expectedCount;
    _recoveryTimer.reset();
    if (expectedCount == 0) {
        _state->allPreparedTxnsResolved.emplaceValue();
    }
}

void ReclaimedPreparedTxnTracker::discoveryComplete() {
    std::lock_guard<std::mutex> lk(_mutex);
    invariant(_discoveryStarted,
              "Completing discovery of reclaimed prepared txns before explicitly starting it");
    invariant(!_discoveryComplete,
              "Completing discovery of reclaimed prepared txns after it has already completed");
    invariant(_remainingToTrack == 0,
              "Did not track the expected number of reclaimed prepared txns");
    _discoveryComplete = true;
    _recoveryDurationMicros = _recoveryTimer.micros();
}

long long ReclaimedPreparedTxnTracker::getNumReclaimedPreparedTxnsRemaining() const {
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_state) {
        return 0;
    }
    return _state->unresolvedPreparedTxnsCount.load();
}

long long ReclaimedPreparedTxnTracker::getRecoveryDurationMicros() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _recoveryDurationMicros;
}

}  // namespace mongo
