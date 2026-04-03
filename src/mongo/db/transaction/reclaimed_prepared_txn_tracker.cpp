/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_discoveryStarted,
              "Attempting to wait on all reclaimed prepared txn before discovery of reclaimed "
              "prepared txns has started");
    invariant(_discoveryComplete,
              "Attempting to wait on all reclaimed prepared txns before discovery of reclaimed "
              "prepared txns is complete");
    return _state->allPreparedTxnsResolved.getFuture();
}

void ReclaimedPreparedTxnTracker::trackPrepareExit(SharedSemiFuture<void> onExitPrepareFuture) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!_state) {
        return 0;
    }
    return _state->unresolvedPreparedTxnsCount.load();
}

long long ReclaimedPreparedTxnTracker::getRecoveryDurationMicros() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _recoveryDurationMicros;
}

}  // namespace mongo
