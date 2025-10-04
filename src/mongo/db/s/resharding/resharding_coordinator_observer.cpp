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


#include "mongo/db/s/resharding/resharding_coordinator_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/assert_util.h"

#include <mutex>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {
namespace {

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
 * Returns true if all participants are in a state greater than or equal to the expectedState.
 * Returns false if the participants list is empty.
 */
template <class TState, class TParticipant>
bool allParticipantsInStateGTE(WithLock lk,
                               TState expectedState,
                               const std::vector<TParticipant>& participants) {
    if (participants.size() == 0) {
        return false;
    }

    for (const auto& shard : participants) {
        if (shard.getMutableState().getState() < expectedState) {
            return false;
        }
    }
    return true;
}

/**
 * Returns whether or not all relevant shards have completed their transitions into the
 * expectedState. If they have, ensures the promise is fulfilled.
 */
template <class TState>
bool stateTransistionsComplete(WithLock lk,
                               SharedPromise<ReshardingCoordinatorDocument>& sp,
                               TState expectedState,
                               const ReshardingCoordinatorDocument& updatedStateDoc) {
    if (sp.getFuture().isReady()) {
        // Ensure promise is not fulfilled twice.
        return true;
    }

    auto participants = getParticipants(lk, expectedState, updatedStateDoc);
    auto allShardsTransitioned = allParticipantsInStateGTE(lk, expectedState, participants);
    if (allShardsTransitioned) {
        sp.emplaceValue(updatedStateDoc);
        return true;
    }
    return false;
}

/**
 * Appends context regarding the source of the abortReason.
 */
template <class TParticipant>
Status getStatusFromAbortReasonWithShardInfo(const TParticipant& participant,
                                             StringData participantType) {
    return resharding::getStatusFromAbortReason(participant.getMutableState())
        .withContext(fmt::format("{} shard {} reached an unrecoverable error",
                                 participantType,
                                 participant.getId().toString()));
}

/**
 * If neither the coordinator nor participants have encountered an unrecoverable error, returns
 * boost::none.
 *
 * Otherwise, returns the abortReason reported by either the coordinator or one of the participants.
 */
boost::optional<Status> getAbortReasonIfExists(
    const ReshardingCoordinatorDocument& updatedStateDoc) {
    if (updatedStateDoc.getAbortReason()) {
        // Note: the absence of context specifying which shard the abortReason originates from
        // implies the abortReason originates from the coordinator.
        return resharding::getStatusFromAbortReason(updatedStateDoc);
    }

    for (const auto& donorShard : updatedStateDoc.getDonorShards()) {
        if (donorShard.getMutableState().getState() == DonorStateEnum::kError) {
            return getStatusFromAbortReasonWithShardInfo(donorShard, "Donor"_sd);
        }
    }

    for (const auto& recipientShard : updatedStateDoc.getRecipientShards()) {
        if (recipientShard.getMutableState().getState() == RecipientStateEnum::kError) {
            return getStatusFromAbortReasonWithShardInfo(recipientShard, "Recipient"_sd);
        }
    }

    return boost::none;
}
}  // namespace

ReshardingCoordinatorObserver::ReshardingCoordinatorObserver() = default;

ReshardingCoordinatorObserver::~ReshardingCoordinatorObserver() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    // Rarely, when there is a short period of time between stepdown and stepup, the
    // ReshardingCoordinator::run() method is not called causing the invariants below
    // to fire. If this method hasn't been called, we skip the checks.
    if (!_reshardingCoordinatorRunCalled) {
        return;
    }
    invariant(_allDonorsReportedMinFetchTimestamp.getFuture().isReady());
    invariant(_allRecipientsFinishedCloning.getFuture().isReady());
    invariant(_allRecipientsReportedStrictConsistencyTimestamp.getFuture().isReady());
    invariant(_allRecipientsDone.getFuture().isReady());
    invariant(_allDonorsDone.getFuture().isReady());
}

void ReshardingCoordinatorObserver::onReshardingParticipantTransition(
    const ReshardingCoordinatorDocument& updatedStateDoc) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (auto abortReason = getAbortReasonIfExists(updatedStateDoc)) {
        _onAbortOrStepdown(lk, abortReason.value());
        // Don't exit early since the coordinator waits for all participants to report state 'done'.
    }

    if (!stateTransistionsComplete(lk,
                                   _allDonorsReportedMinFetchTimestamp,
                                   DonorStateEnum::kDonatingInitialData,
                                   updatedStateDoc)) {
        return;
    }

    if (!stateTransistionsComplete(
            lk, _allRecipientsFinishedCloning, RecipientStateEnum::kApplying, updatedStateDoc)) {
        return;
    }

    if (!stateTransistionsComplete(lk,
                                   _allRecipientsReportedStrictConsistencyTimestamp,
                                   RecipientStateEnum::kStrictConsistency,
                                   updatedStateDoc)) {
        return;
    }

    if (!stateTransistionsComplete(
            lk, _allRecipientsDone, RecipientStateEnum::kDone, updatedStateDoc)) {
        return;
    }

    if (!stateTransistionsComplete(lk, _allDonorsDone, DonorStateEnum::kDone, updatedStateDoc)) {
        return;
    }
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllDonorsReadyToDonate() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _allDonorsReportedMinFetchTimestamp.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsFinishedCloning() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _allRecipientsFinishedCloning.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsInStrictConsistency() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _allRecipientsReportedStrictConsistencyTimestamp.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllDonorsDone() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _allDonorsDone.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsDone() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _allRecipientsDone.getFuture();
}

void ReshardingCoordinatorObserver::interrupt(Status status) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _onAbortOrStepdown(lk, status);

    if (!_allRecipientsDone.getFuture().isReady()) {
        _allRecipientsDone.setError(status);
    }

    if (!_allDonorsDone.getFuture().isReady()) {
        _allDonorsDone.setError(status);
    }
}

void ReshardingCoordinatorObserver::fulfillPromisesBeforePersistingStateDoc() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_allDonorsReportedMinFetchTimestamp.getFuture().isReady());
    invariant(!_allRecipientsFinishedCloning.getFuture().isReady());
    invariant(!_allRecipientsReportedStrictConsistencyTimestamp.getFuture().isReady());
    invariant(!_allRecipientsDone.getFuture().isReady());
    invariant(!_allDonorsDone.getFuture().isReady());
    _allDonorsReportedMinFetchTimestamp.emplaceValue();
    _allRecipientsFinishedCloning.emplaceValue();
    _allRecipientsReportedStrictConsistencyTimestamp.emplaceValue();
    _allRecipientsDone.emplaceValue();
    _allDonorsDone.emplaceValue();
}

void ReshardingCoordinatorObserver::onCriticalSectionTimeout() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!_allRecipientsReportedStrictConsistencyTimestamp.getFuture().isReady()) {
        _allRecipientsReportedStrictConsistencyTimestamp.setError(
            Status{ErrorCodes::ReshardingCriticalSectionTimeout,
                   "Resharding critical section timed out."});
    }
}

void ReshardingCoordinatorObserver::_onAbortOrStepdown(WithLock, Status status) {
    if (!_allDonorsReportedMinFetchTimestamp.getFuture().isReady()) {
        _allDonorsReportedMinFetchTimestamp.setError(status);
    }

    if (!_allRecipientsFinishedCloning.getFuture().isReady()) {
        _allRecipientsFinishedCloning.setError(status);
    }

    if (!_allRecipientsReportedStrictConsistencyTimestamp.getFuture().isReady()) {
        _allRecipientsReportedStrictConsistencyTimestamp.setError(status);
    }
}

}  // namespace mongo
