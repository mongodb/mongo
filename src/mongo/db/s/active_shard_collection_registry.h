
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

#include "mongo/base/disallow_copying.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/notification.h"

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
    MONGO_DISALLOW_COPYING(ActiveShardCollectionRegistry);

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

private:
    friend class ScopedShardCollection;

    // Describes the state of a currently active shardCollection operation
    struct ActiveShardCollectionState {
        ActiveShardCollectionState(ShardsvrShardCollection inRequest)
            : activeRequest(std::move(inRequest)),
              notification(std::make_shared<Notification<Status>>()) {}

        /**
         * Constructs an error status to return in the case of conflicting operations.
         */
        Status constructErrorStatus(const ShardsvrShardCollection& request) const;

        // Exact arguments of the currently active operation
        ShardsvrShardCollection activeRequest;

        // Notification event that will be signaled when the currently active operation completes
        std::shared_ptr<Notification<Status>> notification;
    };

    /**
     * Unregisters a previously registered namespace with an ongoing shardCollection. Must only be
     * called if a previous call to registerShardCollection has succeeded.
     */
    void _clearShardCollection(std::string nss);

    // Protects the state below
    stdx::mutex _mutex;

    // Map containing any collections currently being sharded
    StringMap<ActiveShardCollectionState> _activeShardCollectionMap;
};

/**
 * Object of this class is returned from the registerShardCollection call of the active shard
 * collection registry. It can exist in two modes - 'execute' and 'join'. See the comments for
 * registerShardCollection method for more details.
 */
class ScopedShardCollection {
    MONGO_DISALLOW_COPYING(ScopedShardCollection);

public:
    ScopedShardCollection(std::string nss,
                          ActiveShardCollectionRegistry* registry,
                          bool shouldExecute,
                          std::shared_ptr<Notification<Status>> completionNotification);
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
     * was invoked (the command immediately executed). Signals any callers that might be blocked in
     * waitForCompletion.
     */
    void signalComplete(Status status);

    /**
     * Must only be called if the object is in the 'join' mode. Blocks until the main executor of
     * the shardCollection command calls signalComplete.
     */
    Status waitForCompletion(OperationContext* opCtx);

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

    // This is the future, which will be signaled at the end of shardCollection
    std::shared_ptr<Notification<Status>> _completionNotification;
};

}  // namespace mongo
