// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_donor_promises.h"

#include <utility>

namespace mongo {

namespace {

DonorStateEnum getDonorState(const ReshardingDonorDocument& doc) {
    return doc.getMutableState().getState();
}

}  // namespace

ReshardingDonorPromises::ReshardingDonorPromises()
    : _allRecipientsDoneCloning(_registry,
                                [this](WithLock lk, const ReshardingDonorDocument& doc) {
                                    _recoverAllRecipientsDoneCloning(lk, doc);
                                }),
      _allRecipientsDoneApplying(_registry,
                                 [this](WithLock lk, const ReshardingDonorDocument& doc) {
                                     _recoverAllRecipientsDoneApplying(lk, doc);
                                 }),
      _inDonatingOplogEntries(_registry,
                              [this](WithLock lk, const ReshardingDonorDocument& doc) {
                                  _recoverInDonatingOplogEntries(lk, doc);
                              }),
      _inBlockingWritesOrError(_registry,
                               [this](WithLock lk, const ReshardingDonorDocument& doc) {
                                   _recoverInBlockingWritesOrError(lk, doc);
                               }),
      _critSecWasAcquired(_registry,
                          [this](WithLock lk, const ReshardingDonorDocument& doc) {
                              _recoverCritSecWasAcquired(lk, doc);
                          }),
      _critSecWasPromoted(_registry, [this](WithLock lk, const ReshardingDonorDocument& doc) {
          _recoverCritSecWasPromoted(lk, doc);
      }) {}

void ReshardingDonorPromises::recover(WithLock lk, const ReshardingDonorDocument& doc) {
    _registry.recover(lk, doc);
}

void ReshardingDonorPromises::setError(WithLock lk, Status status) {
    _registry.setError(lk, std::move(status));
}

void ReshardingDonorPromises::onCoordinatorStateAdvanced(WithLock lk,
                                                         CoordinatorStateEnum newState) {
    if (newState >= CoordinatorStateEnum::kApplying) {
        _allRecipientsDoneCloning.emplaceValue(lk);
    }
    if (newState >= CoordinatorStateEnum::kBlockingWrites) {
        _allRecipientsDoneApplying.emplaceValue(lk);
    }
}

void ReshardingDonorPromises::onDonorStateAdvanced(WithLock lk, DonorStateEnum newState) {
    if (newState >= DonorStateEnum::kDonatingOplogEntries) {
        _allRecipientsDoneCloning.emplaceValue(lk);
        _inDonatingOplogEntries.emplaceValue(lk);
    }
    if (newState >= DonorStateEnum::kPreparingToBlockWrites) {
        _allRecipientsDoneApplying.emplaceValue(lk);
    }
    if (newState == DonorStateEnum::kBlockingWrites || newState == DonorStateEnum::kError) {
        _inBlockingWritesOrError.emplaceValue(lk);
    }
    // _critSecWasAcquired and _critSecWasPromoted are emplaced explicitly via the
    // emplaceCritSecWas* hooks because tests pause the on-disk transition without invalidating
    // the in-memory action.
}

void ReshardingDonorPromises::emplaceCritSecWasAcquired(WithLock lk) {
    _critSecWasAcquired.emplaceValue(lk);
}

void ReshardingDonorPromises::emplaceCritSecWasPromoted(WithLock lk) {
    _critSecWasPromoted.emplaceValue(lk);
}

SharedSemiFuture<void> ReshardingDonorPromises::getAllRecipientsDoneCloningFuture() const {
    // coverity[missing_lock]
    return _allRecipientsDoneCloning.getFuture();
}

SharedSemiFuture<void> ReshardingDonorPromises::getAllRecipientsDoneApplyingFuture() const {
    // coverity[missing_lock]
    return _allRecipientsDoneApplying.getFuture();
}

SharedSemiFuture<void> ReshardingDonorPromises::getInDonatingOplogEntriesFuture() const {
    // coverity[missing_lock]
    return _inDonatingOplogEntries.getFuture();
}

SharedSemiFuture<void> ReshardingDonorPromises::getInBlockingWritesOrErrorFuture() const {
    // coverity[missing_lock]
    return _inBlockingWritesOrError.getFuture();
}

SharedSemiFuture<void> ReshardingDonorPromises::getCritSecWasAcquiredFuture() const {
    // coverity[missing_lock]
    return _critSecWasAcquired.getFuture();
}

SharedSemiFuture<void> ReshardingDonorPromises::getCritSecWasPromotedFuture() const {
    // coverity[missing_lock]
    return _critSecWasPromoted.getFuture();
}

void ReshardingDonorPromises::_recoverAllRecipientsDoneCloning(WithLock lk,
                                                               const ReshardingDonorDocument& doc) {
    if (getDonorState(doc) >= DonorStateEnum::kDonatingOplogEntries) {
        _allRecipientsDoneCloning.emplaceValue(lk);
    }
}

void ReshardingDonorPromises::_recoverAllRecipientsDoneApplying(
    WithLock lk, const ReshardingDonorDocument& doc) {
    if (getDonorState(doc) >= DonorStateEnum::kPreparingToBlockWrites) {
        _allRecipientsDoneApplying.emplaceValue(lk);
    }
}

void ReshardingDonorPromises::_recoverInDonatingOplogEntries(WithLock lk,
                                                             const ReshardingDonorDocument& doc) {
    if (getDonorState(doc) >= DonorStateEnum::kDonatingOplogEntries) {
        _inDonatingOplogEntries.emplaceValue(lk);
    }
}

void ReshardingDonorPromises::_recoverInBlockingWritesOrError(WithLock lk,
                                                              const ReshardingDonorDocument& doc) {
    const auto state = getDonorState(doc);
    if (state >= DonorStateEnum::kBlockingWrites || state == DonorStateEnum::kError) {
        _inBlockingWritesOrError.emplaceValue(lk);
    }
}

void ReshardingDonorPromises::_recoverCritSecWasAcquired(WithLock lk,
                                                         const ReshardingDonorDocument& doc) {
    if (getDonorState(doc) >= DonorStateEnum::kBlockingWrites) {
        _critSecWasAcquired.emplaceValue(lk);
    }
}

void ReshardingDonorPromises::_recoverCritSecWasPromoted(WithLock lk,
                                                         const ReshardingDonorDocument& doc) {
    auto mutableState = doc.getMutableState();
    if (getDonorState(doc) == DonorStateEnum::kDone && !mutableState.getAbortReason()) {
        _critSecWasPromoted.emplaceValue(lk);
    }
}

}  // namespace mongo
