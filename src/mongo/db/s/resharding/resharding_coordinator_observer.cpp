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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/resharding/resharding_coordinator_observer.h"

#include <fmt/format.h>

#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_id.h"

namespace mongo {
namespace {

using namespace fmt::literals;

/**
 * Retrieves the participants corresponding to the expectedState type.
 */
const std::vector<DonorShardEntry>& getParticipants(
    WithLock, DonorStateEnum expectedState, const ReshardingCoordinatorDocument& updatedStateDoc) {
    return updatedStateDoc.getDonorShards();
}
const std::vector<RecipientShardEntry>& getParticipants(
    WithLock,
    RecipientStateEnum expectedState,
    const ReshardingCoordinatorDocument& updatedStateDoc) {
    return updatedStateDoc.getRecipientShards();
}

/**
 * Generates error an response indicating which participant was found to be in state kError.
 */
Status generateErrorStatus(WithLock, const DonorShardEntry& donor) {
    return {ErrorCodes::InternalError,
            "Donor shard {} is in an error state"_format(donor.getId().toString())};
}
Status generateErrorStatus(WithLock, const RecipientShardEntry& recipient) {
    return {ErrorCodes::InternalError,
            "Recipient shard {} is in an error state"_format(recipient.getId().toString())};
}

/**
 * Returns true if the participants all have a (non-error) state greater than or equal to
 * expectedState, false otherwise.
 *
 * If any participant is in state kError, returns an error status.
 */
template <class TState, class TParticipant>
StatusWith<bool> allParticipantsInStateGTE(WithLock lk,
                                           TState expectedState,
                                           const std::vector<TParticipant>& participants) {

    bool allInStateGTE = true;
    for (const auto& shard : participants) {
        auto state = shard.getState();
        if (state != expectedState) {
            if (state == TState::kError) {
                return generateErrorStatus(lk, shard);
            }

            // If one state is greater than the expectedState, it is guaranteed that all other
            // participant states are at least expectedState or greater.
            //
            // Instead of early returning, continue loop in case another participant reported an
            // error.
            allInStateGTE = state > expectedState;
        }
    }
    return allInStateGTE;
}

/**
 * Returns true if the state transition is incomplete and the promise cannot yet be fulfilled. This
 * includes if one or more of the participants report state kError. In the error case, an error is
 * set on the promise.
 *
 * Otherwise returns false and fulfills the promise if it is not already.
 */
template <class T>
bool stateTransitionIncomplete(WithLock lk,
                               SharedPromise<ReshardingCoordinatorDocument>& sp,
                               T expectedState,
                               const ReshardingCoordinatorDocument& updatedStateDoc) {
    if (sp.getFuture().isReady()) {
        // Ensure promise is not fulfilled twice.
        return false;
    }

    auto participants = getParticipants(lk, expectedState, updatedStateDoc);
    auto swAllParticipantsGTE = allParticipantsInStateGTE(lk, expectedState, participants);
    if (!swAllParticipantsGTE.isOK()) {
        // By returning true, onReshardingParticipantTransition will not try to fulfill any more of
        // the promises.
        // ReshardingCoordinator::run() waits on the promises from the ReshardingCoordinatorObserver
        // in the same order they're fulfilled by onReshardingParticipantTransition(). If even one
        // promise in the chain errors, ReshardingCoordinator::run() will jump to onError and not
        // wait for the remaining promises to be fulfilled.
        sp.setError(swAllParticipantsGTE.getStatus());
        return true;
    }

    if (swAllParticipantsGTE.getValue()) {
        // All participants are in a state greater than or equal to expected state.
        sp.emplaceValue(updatedStateDoc);
        return false;
    }
    return true;
}

}  // namespace

ReshardingCoordinatorObserver::ReshardingCoordinatorObserver() = default;

ReshardingCoordinatorObserver::~ReshardingCoordinatorObserver() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_allDonorsReportedMinFetchTimestamp.getFuture().isReady());
    invariant(_allRecipientsFinishedCloning.getFuture().isReady());
    invariant(_allRecipientsFinishedApplying.getFuture().isReady());
    invariant(_allRecipientsReportedStrictConsistencyTimestamp.getFuture().isReady());
    invariant(_allRecipientsRenamedCollection.getFuture().isReady());
    invariant(_allDonorsDroppedOriginalCollection.getFuture().isReady());
}

void ReshardingCoordinatorObserver::onReshardingParticipantTransition(
    const ReshardingCoordinatorDocument& updatedStateDoc) {

    stdx::lock_guard<Latch> lk(_mutex);

    if (stateTransitionIncomplete(lk,
                                  _allDonorsReportedMinFetchTimestamp,
                                  DonorStateEnum::kDonatingInitialData,
                                  updatedStateDoc)) {
        return;
    }

    if (stateTransitionIncomplete(
            lk, _allRecipientsFinishedCloning, RecipientStateEnum::kApplying, updatedStateDoc)) {
        return;
    }

    if (stateTransitionIncomplete(lk,
                                  _allRecipientsFinishedApplying,
                                  RecipientStateEnum::kSteadyState,
                                  updatedStateDoc)) {
        return;
    }

    if (stateTransitionIncomplete(lk,
                                  _allRecipientsReportedStrictConsistencyTimestamp,
                                  RecipientStateEnum::kStrictConsistency,
                                  updatedStateDoc)) {
        return;
    }

    if (stateTransitionIncomplete(
            lk, _allRecipientsRenamedCollection, RecipientStateEnum::kDone, updatedStateDoc)) {
        return;
    }

    if (stateTransitionIncomplete(
            lk, _allDonorsDroppedOriginalCollection, DonorStateEnum::kDone, updatedStateDoc)) {
        return;
    }
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllDonorsReadyToDonate() {
    return _allDonorsReportedMinFetchTimestamp.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsFinishedCloning() {
    return _allRecipientsFinishedCloning.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsFinishedApplying() {
    return _allRecipientsFinishedApplying.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsInStrictConsistency() {
    return _allRecipientsReportedStrictConsistencyTimestamp.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllDonorsDroppedOriginalCollection() {
    return _allDonorsDroppedOriginalCollection.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsRenamedCollection() {
    return _allRecipientsRenamedCollection.getFuture();
}

void ReshardingCoordinatorObserver::interrupt(Status status) {
    stdx::lock_guard<Latch> lg(_mutex);

    if (!_allDonorsReportedMinFetchTimestamp.getFuture().isReady()) {
        _allDonorsReportedMinFetchTimestamp.setError(status);
    }

    if (!_allRecipientsFinishedCloning.getFuture().isReady()) {
        _allRecipientsFinishedCloning.setError(status);
    }

    if (!_allRecipientsFinishedApplying.getFuture().isReady()) {
        _allRecipientsFinishedApplying.setError(status);
    }

    if (!_allRecipientsReportedStrictConsistencyTimestamp.getFuture().isReady()) {
        _allRecipientsReportedStrictConsistencyTimestamp.setError(status);
    }

    if (!_allRecipientsRenamedCollection.getFuture().isReady()) {
        _allRecipientsRenamedCollection.setError(status);
    }

    if (!_allDonorsDroppedOriginalCollection.getFuture().isReady()) {
        _allDonorsDroppedOriginalCollection.setError(status);
    }
}

}  // namespace mongo
