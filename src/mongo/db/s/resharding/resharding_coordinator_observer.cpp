// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/resharding/resharding_coordinator_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/assert_util.h"

#include <mutex>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
                                             std::string_view participantType) {
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
            return getStatusFromAbortReasonWithShardInfo(donorShard, "Donor"sv);
        }
    }

    for (const auto& recipientShard : updatedStateDoc.getRecipientShards()) {
        if (recipientShard.getMutableState().getState() == RecipientStateEnum::kError) {
            return getStatusFromAbortReasonWithShardInfo(recipientShard, "Recipient"sv);
        }
    }

    return boost::none;
}
}  // namespace

ReshardingCoordinatorObserver::ReshardingCoordinatorObserver() = default;

ReshardingCoordinatorObserver::~ReshardingCoordinatorObserver() {
    std::lock_guard<std::mutex> lg(_mutex);

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
    std::lock_guard<std::mutex> lk(_mutex);
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
    std::lock_guard<std::mutex> lk(_mutex);
    return _allDonorsReportedMinFetchTimestamp.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsFinishedCloning() {
    std::lock_guard<std::mutex> lk(_mutex);
    return _allRecipientsFinishedCloning.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsInStrictConsistency() {
    std::lock_guard<std::mutex> lk(_mutex);
    return _allRecipientsReportedStrictConsistencyTimestamp.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllDonorsDone() {
    std::lock_guard<std::mutex> lk(_mutex);
    return _allDonorsDone.getFuture();
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorObserver::awaitAllRecipientsDone() {
    std::lock_guard<std::mutex> lk(_mutex);
    return _allRecipientsDone.getFuture();
}

void ReshardingCoordinatorObserver::interrupt(Status status) {
    std::lock_guard<std::mutex> lk(_mutex);
    _onAbortOrStepdown(lk, status);

    if (!_allRecipientsDone.getFuture().isReady()) {
        _allRecipientsDone.setError(status);
    }

    if (!_allDonorsDone.getFuture().isReady()) {
        _allDonorsDone.setError(status);
    }
}

void ReshardingCoordinatorObserver::fulfillPromisesBeforePersistingStateDoc() {
    std::lock_guard<std::mutex> lk(_mutex);
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
    std::lock_guard<std::mutex> lk(_mutex);
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
