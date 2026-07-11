// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_recipient_promises.h"

#include "mongo/db/s/resharding/resharding_util.h"

#include <functional>
#include <utility>

namespace mongo {

namespace {
using Self = ReshardingRecipientPromises;
using RecoverMethod = void (Self::*)(WithLock, const ReshardingRecipientDocument&);
using RecoveryFn = ReshardingPromiseRegistry<ReshardingRecipientDocument>::RecoveryFn;

RecoveryFn makeRecovery(Self* self, RecoverMethod fn) {
    return [self, fn = std::mem_fn(fn)](WithLock lk, const ReshardingRecipientDocument& doc) {
        fn(self, lk, doc);
    };
}
}  // namespace

ReshardingRecipientPromises::ReshardingRecipientPromises()
    : _allDonorsPreparedToDonate(_preDecisionRegistry,
                                 makeRecovery(this, &Self::_recoverAllDonorsPreparedToDonate)),
      _coordinatorBlockingWrites(_preDecisionRegistry,
                                 makeRecovery(this, &Self::_recoverCoordinatorBlockingWrites)),
      _coordinatorCommitted(_postDecisionRegistry,
                            makeRecovery(this, &Self::_recoverCoordinatorCommitted)),
      _inApplyingOrError(_preDecisionRegistry,
                         makeRecovery(this, &Self::_recoverInApplyingOrError)),
      _inStrictConsistencyOrError(_preDecisionRegistry,
                                  makeRecovery(this, &Self::_recoverInStrictConsistencyOrError)),
      _inCreatingCollection(_preDecisionRegistry,
                            makeRecovery(this, &Self::_recoverInCreatingCollection)) {}

void ReshardingRecipientPromises::recover(WithLock lk, const ReshardingRecipientDocument& doc) {
    _preDecisionRegistry.recover(lk, doc);
    _postDecisionRegistry.recover(lk, doc);
}

void ReshardingRecipientPromises::setRecipientError(WithLock lk, Status status) {
    _preDecisionRegistry.setError(lk, status);
}

void ReshardingRecipientPromises::setCoordinatorError(WithLock lk, Status status) {
    _preDecisionRegistry.setError(lk, status);
    _postDecisionRegistry.setError(lk, status);
}

void ReshardingRecipientPromises::setRunnerError(WithLock lk, Status status) {
    _preDecisionRegistry.setError(lk, status);
    _postDecisionRegistry.setError(lk, status);
}

void ReshardingRecipientPromises::onCoordinatorStateAdvanced(
    WithLock lk, CoordinatorStateEnum newState, boost::optional<CloneDetails> cloneDetails) {
    if (newState >= CoordinatorStateEnum::kCloning && cloneDetails) {
        _allDonorsPreparedToDonate.emplaceValue(lk, std::move(*cloneDetails));
    }
    if (newState >= CoordinatorStateEnum::kBlockingWrites) {
        _coordinatorBlockingWrites.emplaceValue(lk);
    }
    if (newState >= CoordinatorStateEnum::kCommitting) {
        _coordinatorCommitted.emplaceValue(lk);
    }
}

void ReshardingRecipientPromises::onRecipientStateAdvanced(WithLock lk,
                                                           RecipientStateEnum newState) {
    if (newState == RecipientStateEnum::kApplying || newState == RecipientStateEnum::kError) {
        _inApplyingOrError.emplaceValue(lk);
    }
    if (newState == RecipientStateEnum::kStrictConsistency ||
        newState == RecipientStateEnum::kError) {
        _inStrictConsistencyOrError.emplaceValue(lk);
    }
    if (newState == RecipientStateEnum::kCreatingCollection) {
        _inCreatingCollection.emplaceValue(lk);
    }
}

SharedSemiFuture<ReshardingRecipientPromises::CloneDetails>
ReshardingRecipientPromises::getAllDonorsPreparedToDonateFuture() const {
    // coverity[missing_lock]
    return _allDonorsPreparedToDonate.getFuture();
}

SharedSemiFuture<void> ReshardingRecipientPromises::getCoordinatorBlockingWritesFuture() const {
    // coverity[missing_lock]
    return _coordinatorBlockingWrites.getFuture();
}

SharedSemiFuture<void> ReshardingRecipientPromises::getCoordinatorCommittedFuture() const {
    // coverity[missing_lock]
    return _coordinatorCommitted.getFuture();
}

void ReshardingRecipientPromises::fulfillCoordinatorCommit(WithLock lk) {
    _coordinatorCommitted.emplaceValue(lk);
}

SharedSemiFuture<void> ReshardingRecipientPromises::getInApplyingOrErrorFuture() const {
    // coverity[missing_lock]
    return _inApplyingOrError.getFuture();
}

SharedSemiFuture<void> ReshardingRecipientPromises::getInStrictConsistencyOrErrorFuture() const {
    // coverity[missing_lock]
    return _inStrictConsistencyOrError.getFuture();
}

SharedSemiFuture<void> ReshardingRecipientPromises::getInCreatingCollectionFuture() const {
    // coverity[missing_lock]
    return _inCreatingCollection.getFuture();
}

void ReshardingRecipientPromises::_recoverCoordinatorBlockingWrites(
    WithLock lk, const ReshardingRecipientDocument& doc) {
    switch (doc.getMutableState().getState()) {
        case RecipientStateEnum::kStrictConsistency:
            // The coordinator must have reached kBlockingWrites before the recipient could enter
            // kStrictConsistency — that is what unblocks the transition.
            _coordinatorBlockingWrites.emplaceValue(lk);
            break;
        case RecipientStateEnum::kDone:
            // It cannot be determined from the recipient's state alone whether the coordinator
            // reached kBlockingWrites before kDone. The recipient may have entered kDone via
            // kError, and may have entered kError either before kBlockingWrites (e.g. during
            // cloning) or after (i.e. during the final catch-up period of applying). Because it's
            // ambiguous whether kBlockingWrites was ever reached when stepping up in kDone,
            // inspecting the state of this promise from kDone is a logic error. We fulfill it with
            // an unrecoverable error to expose this.
            _coordinatorBlockingWrites.setError(
                lk,
                {ErrorCodes::InternalError,
                 "Inspecting this promise from kDone is undefined behavior"});
            break;
        default:
            // For all other states (including kError), the coordinator has not necessarily reached
            // kBlockingWrites, and if it did, it cannot be sure the recipient was made aware and
            // must retry. Leave the promise unfulfilled; we are guaranteed that we'll be informed
            // later via the ordinary path once kBlockingWrites is reached.
            break;
    }
}

void ReshardingRecipientPromises::_recoverCoordinatorCommitted(
    WithLock lk, const ReshardingRecipientDocument& doc) {
    switch (doc.getMutableState().getState()) {
        case RecipientStateEnum::kDone:
            // The coordinator must have persisted a decision for the recipient to reach kDone.
            // The presence of an abortReason distinguishes an abort decision from a commit.
            if (doc.getMutableState().getAbortReason()) {
                _coordinatorCommitted.setError(lk, resharding::kCoordinatorAbortedError);
            } else {
                _coordinatorCommitted.emplaceValue(lk);
            }
            break;
        default:
            // For all other states, the coordinator may not have made a decision yet, or if it
            // did, it cannot be sure the recipient was made aware and must retry. Leave the
            // promise unfulfilled; we are guaranteed that we'll be informed later via the ordinary
            // path once a decision is made.
            break;
    }
}

void ReshardingRecipientPromises::_recoverInCreatingCollection(
    WithLock lk, const ReshardingRecipientDocument& doc) {
    if (doc.getMutableState().getState() >= RecipientStateEnum::kCreatingCollection) {
        _inCreatingCollection.emplaceValue(lk);
    }
}

void ReshardingRecipientPromises::_recoverInStrictConsistencyOrError(
    WithLock lk, const ReshardingRecipientDocument& doc) {
    if (doc.getMutableState().getState() >= RecipientStateEnum::kError) {
        _inStrictConsistencyOrError.emplaceValue(lk);
    }
}

void ReshardingRecipientPromises::_recoverInApplyingOrError(
    WithLock lk, const ReshardingRecipientDocument& doc) {
    if (doc.getMutableState().getState() >= RecipientStateEnum::kApplying) {
        _inApplyingOrError.emplaceValue(lk);
    }
}

void ReshardingRecipientPromises::_recoverAllDonorsPreparedToDonate(
    WithLock lk, const ReshardingRecipientDocument& doc) {
    if (doc.getMutableState().getState() <= RecipientStateEnum::kAwaitingFetchTimestamp) {
        return;
    }
    auto cloneTimestamp = doc.getCloneTimestamp();
    auto metrics = doc.getMetrics();
    if (!cloneTimestamp || !metrics) {
        return;
    }
    if (!metrics->getApproxDocumentsToCopy() || !metrics->getApproxBytesToCopy()) {
        return;
    }
    _allDonorsPreparedToDonate.emplaceValue(lk,
                                            CloneDetails{*cloneTimestamp,
                                                         *metrics->getApproxDocumentsToCopy(),
                                                         *metrics->getApproxBytesToCopy(),
                                                         doc.getDonorShards()});
}

}  // namespace mongo
