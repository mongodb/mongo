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
