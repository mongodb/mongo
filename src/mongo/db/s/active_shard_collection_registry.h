/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/platform/mutex.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/string_map.h"

namespace mongo {

class OperationContext;
class ScopedShardCollection;
template <typename T>
class StatusWith;

/**
 * Thread-safe object that keeps track of any active shardCollection commands running. There is only
 * one instance of this object per shard.
 */
class ActiveShardCollectionRegistry {
    ActiveShardCollectionRegistry(const ActiveShardCollectionRegistry&) = delete;
    ActiveShardCollectionRegistry& operator=(const ActiveShardCollectionRegistry&) = delete;

public:
    ActiveShardCollectionRegistry();
    ~ActiveShardCollectionRegistry();

    static ActiveShardCollectionRegistry& get(ServiceContext* service);
    static ActiveShardCollectionRegistry& get(OperationContext* opCtx);

    /**
     * If the collection being requested to shard is not already being sharded on this shard,
     * registers an active shardCollection with the specified arguments. Returns a
     * ScopedShardCollection object, which must be signaled by the caller before it goes out of
     * scope.
     *
     * If this collection is already being sharded on this shard and it has the exact same
     * arguments, returns a ScopedShardCollection. The ScopedShardCollection can be used to join the
     * already running shard collection.
     *
     * Otherwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedShardCollection> registerShardCollection(
        const ShardsvrShardCollection& request);

    /**
     * Takes a snapshot of all currently active shard collections and synchronously waits for each
     * to complete.
     *
     * TODO SERVER-44034: Remove this method.
     */
    void waitForActiveShardCollectionsToComplete(OperationContext* opCtx);

private:
    friend class ScopedShardCollection;

    // Describes the state of a currently active shardCollection operation
    struct ActiveShardCollectionState {
        ActiveShardCollectionState(ShardsvrShardCollection inRequest)
            : activeRequest(std::move(inRequest)) {}

        /**
         * Constructs an error status to return in the case of conflicting operations.
         */
        Status constructErrorStatus(const ShardsvrShardCollection& request) const;

        // Exact arguments of the currently active operation
        ShardsvrShardCollection activeRequest;

        /**
         * Promise that contains the uuid for this collection so that a shardCollection object
         * that is in 'join' mode has access to the collection uuid.
         */
        SharedPromise<boost::optional<UUID>> _uuidPromise;
    };

    /**
     * Unregisters a previously registered namespace with an ongoing shardCollection. Must only be
     * called if a previous call to registerShardCollection has succeeded.
     */
    void _clearShardCollection(std::string nss);

    // Fulfills the promise and stores the uuid for the collection if the status is OK or sets an
    // error on the promise if it is not.
    void _setUUIDOrError(std::string nss, StatusWith<boost::optional<UUID>> swUUID);

    // Protects the state below
    Mutex _mutex = MONGO_MAKE_LATCH("ActiveShardCollectionRegistry::_mutex");

    // Map containing any collections currently being sharded
    StringMap<std::shared_ptr<ActiveShardCollectionState>> _activeShardCollectionMap;
};

/**
 * Object of this class is returned from the registerShardCollection call of the active shard
 * collection registry. It can exist in two modes - 'execute' and 'join'. See the comments for
 * registerShardCollection method for more details.
 */
class ScopedShardCollection {
    ScopedShardCollection(const ScopedShardCollection&) = delete;
    ScopedShardCollection& operator=(const ScopedShardCollection&) = delete;

public:
    ScopedShardCollection(std::string nss,
                          ActiveShardCollectionRegistry* registry,
                          bool shouldExecute,
                          SharedSemiFuture<boost::optional<UUID>> uuidFuture);
    ~ScopedShardCollection();

    ScopedShardCollection(ScopedShardCollection&&);
    ScopedShardCollection& operator=(ScopedShardCollection&&);

    /**
     * Returns true if the shardCollection object is in the 'execute' mode. This means that the
     * caller can execute the shardCollection command. The holder must execute the command and call
     * signalComplete with a status.
     */
    bool mustExecute() const {
        return _shouldExecute;
    }

    /**
     * Must only be called if the object is in the 'execute' mode when the shardCollection command
     * was invoked (the command immediately executed). Will either emplace the uuid on the promise
     * stored in the ActiveShardCollectionRegistry for this nss if status is OK or sets an error if
     * it is not.
     */
    void emplaceUUID(StatusWith<boost::optional<UUID>> swUUID);

    /**
     * Must only be called if the object is in the 'join' mode. Gets a future that contains the uuid
     * for the collection.
     */
    SharedSemiFuture<boost::optional<UUID>> getUUID();

private:
    // Namespace of collection being sharded
    std::string _nss;

    // Registry from which to unregister the migration. Not owned.
    ActiveShardCollectionRegistry* _registry;

    /**
     * Whether the holder is the first in line for a newly started shardCollection (in which case
     * the destructor must unregister) or the caller is joining on an already-running
     * shardCollection (in which case the caller must block and wait for completion).
     */
    bool _shouldExecute;

    // Future that will be signaled at the end of shardCollection, contains the uuid for the
    // collection
    SharedSemiFuture<boost::optional<UUID>> _uuidFuture;
};

}  // namespace mongo
