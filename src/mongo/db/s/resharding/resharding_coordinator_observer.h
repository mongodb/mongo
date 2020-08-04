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
     * Called when each recipient updates its 'state' field to 'initializing' in the
     * 'recipientShards' field in a config.reshardingOperations entry.
     */
    void onRecipientReportsCreatedCollection(const ShardId& recipient);

    /**
     * Called when each donor updates its 'state' field to 'donating' and writes its
     * 'minFetchTimestamp' in the 'donorShards' field in a config.reshardingOperations entry.
     */
    void onDonorReportsMinFetchTimestamp(const ShardId& donor, Timestamp timestamp);

    /**
     * Called when each recipient updates its 'state' field to 'steady-state' in the
     * 'recipientShards' field in a config.reshardingOperations entry.
     */
    void onRecipientFinishesCloning(const ShardId& recipient);

    /**
     * Called when each recipient updates its 'state' field to 'strictly-consistent' and writes its
     * 'strictConsistencyTimestamp' in the 'recipientShards' field in a config.reshardingOperations
     * entry.
     */
    void onRecipientReportsStrictlyConsistent(const ShardId& recipient);

    /**
     * Called when each recipient updates its 'state' field to 'done' in the 'recipientShards'
     * field in a config.reshardingOperations entry.
     */
    void onRecipientRenamesCollection(const ShardId& recipient);

    /**
     * Called when each donor updates its 'state' field to 'done' in the 'donorShards' field in
     * a config.reshardingOperations entry.
     */
    void onDonorDropsOriginalCollection(const ShardId& donor);

    /**
     * Called if a recipient reports an unrecoverable error.
     */
    void onRecipientReportsUnrecoverableError(const ShardId& recipient, Status error);

    /**
     * Fulfills the '_allRecipientsCreatedCollection' promise when the last recipient writes that it
     * is in 'initializing' state.
     */
    SharedSemiFuture<void> awaitAllRecipientsCreatedCollection();

    /**
     * When the last donor reports its 'minFetchTimestamp', selects the highest 'minFetchTimestamp'
     * of all donors to be the 'fetchTimestamp'. Fulfills the '_allDonorsReportedMinFetchTimestamp'
     * promise with this 'fetchTimestamp'.
     */
    SharedSemiFuture<Timestamp> awaitAllDonorsReadyToDonate();

    /**
     * Fulfills the '_allRecipientsFinishedCloning' promise when the last recipient writes that it
     * is in 'steady-state'.
     */
    SharedSemiFuture<void> awaitAllRecipientsFinishedCloning();

    /**
     * Fulfills the '_allRecipientsReportedStrictConsistencyTimestamp' promise when the last
     * recipient writes that it is in 'strict-consistency' state as well as its
     * 'strictConsistencyTimestamp'.
     */
    SharedSemiFuture<void> awaitAllRecipientsInStrictConsistency();

    /**
     * Fulfills the '_allDonorsDroppedOriginalCollection' promise when the last donor writes that it
     * is in 'done' state.
     */
    SharedSemiFuture<void> awaitAllDonorsDroppedOriginalCollection();

    /**
     * Fulfills the '_allRecipientsRenamedCollection' promise when the last recipient writes
     * that it is in 'done' state.
     */
    SharedSemiFuture<void> awaitAllRecipientsRenamedCollection();


private:
    // Protects the state below
    Mutex _mutex = MONGO_MAKE_LATCH("ReshardingCoordinatorObserver::_mutex");

    /**
     * Promises indicating that either all donors or all recipients have entered a specific state.
     * The ReshardingCoordinator waits on these in order to transition states.
     */
    SharedPromise<void> _allRecipientsCreatedCollection;

    SharedPromise<Timestamp> _allDonorsReportedMinFetchTimestamp;

    SharedPromise<void> _allRecipientsFinishedCloning;

    SharedPromise<void> _allRecipientsReportedStrictConsistencyTimestamp;

    SharedPromise<void> _allDonorsDroppedOriginalCollection;

    SharedPromise<void> _allRecipientsRenamedCollection;
};

}  // namespace mongo
