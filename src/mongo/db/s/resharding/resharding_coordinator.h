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
#include "mongo/platform/mutex.h"

namespace mongo {

class ChunkType;
class NamespaceString;
class OperationContext;
class ServiceContext;
class ShardId;

/**
 * Manages active resharding operations and holds a catalog of ReshardingCoordinatorStateMachine
 * objects indexed by collection namespace.
 *
 * There is one instance of this object per service context.
 */

class ReshardingCoordinator {
    ReshardingCoordinator(const ReshardingCoordinator&) = delete;
    ReshardingCoordinator& operator=(const ReshardingCoordinator&) = delete;

public:
    ReshardingCoordinator();
    ~ReshardingCoordinator();

    static ReshardingCoordinator& get(ServiceContext* serviceContext);
    static ReshardingCoordinator& get(OperationContext* opCtx);

    /**
     * Creates a new ReshardingCoordinatorStateMachine for the resharding operation on the
     * collection 'nss' and adds it to _reshardingOperationsInProgress. Updates on-disk metadata to
     * indicate that the collection is being resharded.
     */
    void onNewReshardCollection(const NamespaceString& nss,
                                const std::vector<ShardId>& donors,
                                const std::vector<ShardId>& recipients,
                                const std::vector<ChunkType>& initialChunks);

    /**
     * Called when each recipient reports it has finished creating the collection. Triggers a state
     * transition and updates on-disk metadata if this recipient is the last to report it has
     * finished creating the collection.
     */
    void onRecipientReportsCreatedCollection(const NamespaceString& nss, const ShardId& recipient);

    /**
     * Called when each donor reports its minFetchTimestamp. If this donor is the last to report its
     * minFetchTimestamp, selects the highest timestamp among all donors to be the fetchTimestamp,
     * triggers a state change, and updates on-disk metadata.
     */
    void onDonorReportsMinFetchTimestamp(const NamespaceString& nss,
                                         const ShardId& donor,
                                         Timestamp timestamp);

    /**
     * Called when each recipient finishes cloning and enters steady-state. Triggers a state
     * transition and updates on-disk metadata if this recipient is the last to report it has
     * finished cloning the collection.
     */
    void onRecipientFinishesCloning(const NamespaceString& nss, const ShardId& recipient);

    /**
     * Called when a recipient has met the following criteria:
     * 1. Has been notified by all donors all writes must be run in transactions and
     * 2. Has applied all oplog entries and
     * 3. Has entered strict-consistency state
     *
     * If the recipient is the last to report it is in strict-consistency, commits the resharding
     * operation, triggers a state transition, and updates on-disk metadata.
     */
    void onRecipientReportsStrictlyConsistent(const NamespaceString& nss, const ShardId& recipient);

    /**
     * Called when each recipient finishes renaming the temporary collection. Triggers a state
     * transition and updates on-disk metadata if this recipient is the last to report it has
     * finished renaming.
     */
    void onRecipientRenamesCollection(const NamespaceString& nss, const ShardId& recipient);

    /**
     * Called when each donor finishes dropping the original collection. Triggers a state transition
     * and updates on-disk metadata if this donor is the last to report it has finished dropping.
     */
    void onDonorDropsOriginalCollection(const NamespaceString& nss, const ShardId& donor);

    /**
     * Called if a recipient reports an unrecoverable error.
     */
    void onRecipientReportsUnrecoverableError(const NamespaceString& nss,
                                              const ShardId& recipient,
                                              Status error);

private:
    // Protects the state below
    Mutex _mutex = MONGO_MAKE_LATCH("ReshardingCoordinator::_mutex");

    // Contains ReshardingCoordinatorStateMachine objects by collection namespace.
    // TODO SERVER-49569 uncomment line below
    // StringMap<ReshardingCoordinatorStateMachine> _reshardingOperationsInProgress;
};

}  // namespace mongo
