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
#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_promise.h"
#include "mongo/db/s/resharding/resharding_promise_registry.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"

#include <cstdint>
#include <tuple>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Owns all ReshardingPromise instances for the recipient state machine and drives their
 * fulfillment as the coordinator or recipient advances through its state machine.
 *
 * Promises are fulfilled via onCoordinatorStateAdvanced(), called whenever the coordinator's
 * persisted state changes, and via onRecipientStateAdvanced(), called after the recipient
 * majority commits a new state. Currently managed promises:
 *   _allDonorsPreparedToDonate    — fulfilled when the coordinator reaches kCloning and
 *                                   CloneDetails (clone timestamp, document/byte counts,
 *                                   and per-donor fetch timestamps) are available.
 *   _inApplyingOrError            — fulfilled after the recipient majority commits
 *                                   kApplying or kError.
 *   _inStrictConsistencyOrError   — fulfilled after the recipient majority commits
 *                                   kStrictConsistency or kError.
 *   _transitionedToCreateCollection — fulfilled after the recipient majority commits
 *                                     kCreatingCollection.
 *
 * CloneDetails packages the data the recipient needs to begin cloning: the clone timestamp,
 * approximate document and byte counts, and the per-donor fetch timestamps.
 *
 * Lifecycle:
 *   1. RecipientStateMachine constructs ReshardingRecipientPromises as a member; each
 *      ReshardingPromise self-registers with the internal _registry at this point.
 *   2. After a failover, recover() is called once with the reloaded state document to
 *      re-fulfill any promises whose milestones were already reached in the previous term.
 *   3. onCoordinatorStateAdvanced() is called each time the coordinator's state advances;
 *      it fulfills whichever promises correspond to the new coordinator state.
 *   4. onRecipientStateAdvanced() is called each time the recipient majority commits a new
 *      state; it fulfills whichever promises correspond to the new recipient state.
 *   5. On any terminal error, setError() propagates the error to all unfulfilled promises
 *      via the registry.
 *
 * Thread safety: all methods that accept WithLock expect the caller to hold
 * RecipientStateMachine::_mutex.
 */
class ReshardingRecipientPromises {
public:
    struct CloneDetails {
        Timestamp cloneTimestamp;
        int64_t approxDocumentsToCopy;
        int64_t approxBytesToCopy;
        std::vector<DonorShardFetchTimestamp> donorShards;

        auto lens() const {
            return std::tie(cloneTimestamp, approxDocumentsToCopy, approxBytesToCopy);
        }

        friend bool operator==(const CloneDetails& a, const CloneDetails& b) {
            return a.lens() == b.lens();
        }

        friend bool operator!=(const CloneDetails& a, const CloneDetails& b) {
            return a.lens() != b.lens();
        }
    };

    ReshardingRecipientPromises();

    ReshardingRecipientPromises(const ReshardingRecipientPromises&) = delete;
    ReshardingRecipientPromises& operator=(const ReshardingRecipientPromises&) = delete;
    ReshardingRecipientPromises(ReshardingRecipientPromises&&) noexcept = delete;
    ReshardingRecipientPromises& operator=(ReshardingRecipientPromises&&) noexcept = delete;

    void recover(WithLock lk, const ReshardingRecipientDocument& doc);
    void setError(WithLock lk, Status status);

    void onCoordinatorStateAdvanced(WithLock lk,
                                    CoordinatorStateEnum newState,
                                    boost::optional<CloneDetails> cloneDetails);

    void onRecipientStateAdvanced(WithLock lk, RecipientStateEnum newState);

    SharedSemiFuture<CloneDetails> getAllDonorsPreparedToDonateFuture() const;
    SharedSemiFuture<void> getInApplyingOrErrorFuture() const;
    SharedSemiFuture<void> getInStrictConsistencyOrErrorFuture() const;
    SharedSemiFuture<void> getTransitionedToCreateCollectionFuture() const;

private:
    void _recoverAllDonorsPreparedToDonate(WithLock lk, const ReshardingRecipientDocument& doc);
    void _recoverInApplyingOrError(WithLock lk, const ReshardingRecipientDocument& doc);
    void _recoverInStrictConsistencyOrError(WithLock lk, const ReshardingRecipientDocument& doc);
    void _recoverTransitionedToCreateCollection(WithLock lk,
                                                const ReshardingRecipientDocument& doc);

    ReshardingPromiseRegistry<ReshardingRecipientDocument> _registry;
    ReshardingPromise<CloneDetails> _allDonorsPreparedToDonate;
    ReshardingPromise<void> _inApplyingOrError;
    ReshardingPromise<void> _inStrictConsistencyOrError;
    ReshardingPromise<void> _transitionedToCreateCollection;
};

}  // namespace mongo
