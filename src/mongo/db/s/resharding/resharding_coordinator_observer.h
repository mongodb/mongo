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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/string_map.h"

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
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        _reshardingCoordinatorRunCalled = true;
    }

private:
    /**
     * Does work necessary for both recoverable errors (failover/stepdown) and unrecoverable errors
     * (abort resharding).
     */
    void _onAbortOrStepdown(WithLock, Status status);

    // Protects the state below
    stdx::mutex _mutex;

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
