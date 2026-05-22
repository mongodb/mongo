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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_promise.h"
#include "mongo/db/s/resharding/resharding_promise_registry.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"

namespace mongo {

/**
 * Owns the donor-state-machine workflow promises and drives their fulfillment as the coordinator
 * and the local donor advance through their respective state machines.
 *
 * Managed promises (all SharedPromise<void>):
 *   - _allRecipientsDoneCloning : recipients reached kApplying (so cloning is done), OR the donor
 *                                 itself reached kDonatingOplogEntries
 *   - _allRecipientsDoneApplying: recipients reached kBlockingWrites, OR the donor itself reached
 *                                 kPreparingToBlockWrites
 *   - _inDonatingOplogEntries   : donor reached kDonatingOplogEntries
 *   - _inBlockingWritesOrError  : donor reached kBlockingWrites OR kError
 *   - _critSecWasAcquired       : donor reached kBlockingWrites (critical section is acquired by
 *                                 the time the donor finishes writing its blocking-writes oplog
 *                                 entry and transitions)
 *   - _critSecWasPromoted       : donor reached kDone via the success path (no abort reason)
 *
 * Lifecycle:
 *   1. DonorStateMachine constructs ReshardingDonorPromises as a member; each ReshardingPromise
 *      self-registers with the internal _registry at construction time.
 *   2. After a failover, recover() is called once with the reloaded state document so that any
 *      promise whose milestone is already reflected in the persisted donor state is fulfilled
 *      immediately and consumers don't block on milestones that already happened in a prior term.
 *   3. As the coordinator's state advances, onCoordinatorStateAdvanced() fulfills the
 *      coordinator-driven promises (_allRecipientsDoneCloning / _allRecipientsDoneApplying).
 *   4. As the local donor's state advances, onDonorStateAdvanced() fulfills the donor-driven
 *      promises. Cascading is intentional (e.g. reaching kPreparingToBlockWrites also implies
 *      kDonatingOplogEntries was reached) and each onXxxStateAdvanced call is idempotent.
 *   5. On any terminal error, setError() broadcasts the status to every promise that has not yet
 *      been fulfilled via the registry, so no consumer blocks forever.
 *
 * Thread safety: every method takes WithLock. Callers must hold DonorStateMachine::_mutex.
 */
class ReshardingDonorPromises {
public:
    ReshardingDonorPromises();

    ReshardingDonorPromises(const ReshardingDonorPromises&) = delete;
    ReshardingDonorPromises& operator=(const ReshardingDonorPromises&) = delete;
    ReshardingDonorPromises(ReshardingDonorPromises&&) noexcept = delete;
    ReshardingDonorPromises& operator=(ReshardingDonorPromises&&) noexcept = delete;

    void recover(WithLock lk, const ReshardingDonorDocument& doc);
    void setError(WithLock lk, Status status);

    void onCoordinatorStateAdvanced(WithLock lk, CoordinatorStateEnum newState);
    void onDonorStateAdvanced(WithLock lk, DonorStateEnum newState);

    // Explicit emplace hooks for promises whose fulfillment timing does not line up with the
    // donor's state-document transition. The critical-section promises are emplaced once the
    // corresponding action (acquire / promote) completes, which is interleaved with — but not
    // the same as — the on-disk state advance. Drivers (e.g. failpoints used in tests) can pause
    // the state transition itself without invalidating the in-memory acquisition.
    void emplaceCritSecWasAcquired(WithLock lk);
    void emplaceCritSecWasPromoted(WithLock lk);

    SharedSemiFuture<void> getAllRecipientsDoneCloningFuture() const;
    SharedSemiFuture<void> getAllRecipientsDoneApplyingFuture() const;
    SharedSemiFuture<void> getInDonatingOplogEntriesFuture() const;
    SharedSemiFuture<void> getInBlockingWritesOrErrorFuture() const;
    SharedSemiFuture<void> getCritSecWasAcquiredFuture() const;
    SharedSemiFuture<void> getCritSecWasPromotedFuture() const;

private:
    void _recoverAllRecipientsDoneCloning(WithLock lk, const ReshardingDonorDocument& doc);
    void _recoverAllRecipientsDoneApplying(WithLock lk, const ReshardingDonorDocument& doc);
    void _recoverInDonatingOplogEntries(WithLock lk, const ReshardingDonorDocument& doc);
    void _recoverInBlockingWritesOrError(WithLock lk, const ReshardingDonorDocument& doc);
    void _recoverCritSecWasAcquired(WithLock lk, const ReshardingDonorDocument& doc);
    void _recoverCritSecWasPromoted(WithLock lk, const ReshardingDonorDocument& doc);

    // The registry must be declared before any ReshardingPromise members so that each promise's
    // self-registration in its constructor sees a fully constructed registry.
    ReshardingPromiseRegistry<ReshardingDonorDocument> _registry;

    ReshardingPromise<void> _allRecipientsDoneCloning;
    ReshardingPromise<void> _allRecipientsDoneApplying;
    ReshardingPromise<void> _inDonatingOplogEntries;
    ReshardingPromise<void> _inBlockingWritesOrError;
    ReshardingPromise<void> _critSecWasAcquired;
    ReshardingPromise<void> _critSecWasPromoted;
};

}  // namespace mongo
