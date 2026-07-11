// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <mutex>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {

class ChunkType;
class NamespaceString;
class OperationContext;
class ServiceContext;
class ShardId;

/**
 * Observes writes that indicate state changes for a resharding operation. Holds promises that
 * the ReshardingCoordinator waits on in order to transition to a new state.
 *
 * An instance of this object is specific to one resharding operation.
 */
class ReshardingCoordinatorObserver {
public:
    ReshardingCoordinatorObserver();
    ~ReshardingCoordinatorObserver();

    /**
     * Called when a donor or recipient writes to its 'state' field. The updatedStateDoc is the
     * post-image of the ReshardingCoordinatorDocument after the update.
     */
    void onReshardingParticipantTransition(const ReshardingCoordinatorDocument& updatedStateDoc);

    /**
     * When the last donor reports its 'minFetchTimestamp', selects the highest 'minFetchTimestamp'
     * of all donors to be the 'fetchTimestamp'. Fulfills the '_allDonorsReportedMinFetchTimestamp'
     * promise with this 'fetchTimestamp'.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllDonorsReadyToDonate();

    /**
     * Fulfills the '_allRecipientsFinishedCloning' promise when the last recipient writes that it
     * has finished cloning the collection and is ready to start applying oplog entries missed
     * during collection cloning.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllRecipientsFinishedCloning();

    /**
     * Fulfills the '_allRecipientsReportedStrictConsistencyTimestamp' promise when the last
     * recipient writes that it is in 'strict-consistency' state as well as its
     * 'strictConsistencyTimestamp'.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllRecipientsInStrictConsistency();

    /**
     * Fulfills the '_allRecipientsDone' promise when the last recipient writes
     * that it is in 'done' state.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllRecipientsDone();

    /**
     * Fulfills the '_allDonorsDone' promise when the last donor writes that it
     * is in 'done' state.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllDonorsDone();

    /**
     * Checks if all recipients are in steady state. Otherwise, sets an error state so that
     * resharding is aborted.
     */
    void onCriticalSectionTimeout();

    /**
     * Sets errors on any promises that have not yet been fulfilled.
     */
    void interrupt(Status status);

    /**
     * Fulfills all promises prematurely. To be called only if no state document has been persisted
     * yet.
     */
    void fulfillPromisesBeforePersistingStateDoc();

    /**
     * Indicates that the ReshardingCoordinator::run method has been called.
     */
    void reshardingCoordinatorRunCalled() {
        std::lock_guard<std::mutex> lg(_mutex);
        _reshardingCoordinatorRunCalled = true;
    }

private:
    /**
     * Does work necessary for both recoverable errors (failover/stepdown) and unrecoverable errors
     * (abort resharding).
     */
    void _onAbortOrStepdown(WithLock, Status status);

    // Protects the state below
    std::mutex _mutex;

    /**
     * Promises indicating that either all donors or all recipients have entered a specific state.
     * The ReshardingCoordinator waits on these in order to transition states. Promises must be
     * fulfilled in descending order.
     *
     * Below are the relationships between promise and expected state in
     * format: {promiseToFulfill, expectedState}
     *
     *  {_allDonorsReportedMinFetchTimestamp, DonorStateEnum::kDonatingInitialData}
     *  {_allRecipientsFinishedCloning, RecipientStateEnum::kApplying}
     *  {_allRecipientsReportedStrictConsistencyTimestamp, RecipientStateEnum::kStrictConsistency}
     *  {_allRecipientsDone, RecipientStateEnum::kDone}
     *  {_allDonorsDone, DonorStateEnum::kDone}
     */

    SharedPromise<ReshardingCoordinatorDocument> _allDonorsReportedMinFetchTimestamp;

    SharedPromise<ReshardingCoordinatorDocument> _allRecipientsFinishedCloning;

    SharedPromise<ReshardingCoordinatorDocument> _allRecipientsReportedStrictConsistencyTimestamp;

    SharedPromise<ReshardingCoordinatorDocument> _allRecipientsDone;

    SharedPromise<ReshardingCoordinatorDocument> _allDonorsDone;

    // Tracks whether the ReshardingCoordinator::run method has been called.
    bool _reshardingCoordinatorRunCalled = false;
};

}  // namespace mongo
