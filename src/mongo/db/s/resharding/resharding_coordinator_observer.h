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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/string_map.h"

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
     * Fulfills the '_allRecipientsFinishedApplying' promise when the last recipient writes that it
     * is in 'steady-state'.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllRecipientsFinishedApplying();

    /**
     * Fulfills the '_allRecipientsReportedStrictConsistencyTimestamp' promise when the last
     * recipient writes that it is in 'strict-consistency' state as well as its
     * 'strictConsistencyTimestamp'.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllRecipientsInStrictConsistency();

    /**
     * Fulfills the '_allRecipientsRenamedCollection' promise when the last recipient writes
     * that it is in 'done' state.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllRecipientsRenamedCollection();

    /**
     * Fulfills the '_allDonorsDroppedOriginalCollection' promise when the last donor writes that it
     * is in 'done' state.
     */
    SharedSemiFuture<ReshardingCoordinatorDocument> awaitAllDonorsDroppedOriginalCollection();

    /**
     * Sets errors on any promises that have not yet been fulfilled.
     */
    void interrupt(Status status);

private:
    // Protects the state below
    Mutex _mutex = MONGO_MAKE_LATCH("ReshardingCoordinatorObserver::_mutex");

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
     *  {_allRecipientsFinishedApplying, RecipientStateEnum::kSteadyState}
     *  {_allRecipientsReportedStrictConsistencyTimestamp, RecipientStateEnum::kStrictConsistency}
     *  {_allRecipientsRenamedCollection, RecipientStateEnum::kDone}
     *  {_allDonorsDroppedOriginalCollection, DonorStateEnum::kDone}
     */

    SharedPromise<ReshardingCoordinatorDocument> _allDonorsReportedMinFetchTimestamp;

    SharedPromise<ReshardingCoordinatorDocument> _allRecipientsFinishedCloning;

    SharedPromise<ReshardingCoordinatorDocument> _allRecipientsFinishedApplying;

    SharedPromise<ReshardingCoordinatorDocument> _allRecipientsReportedStrictConsistencyTimestamp;

    SharedPromise<ReshardingCoordinatorDocument> _allRecipientsRenamedCollection;

    SharedPromise<ReshardingCoordinatorDocument> _allDonorsDroppedOriginalCollection;
};

}  // namespace mongo
