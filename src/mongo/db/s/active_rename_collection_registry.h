/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/s/request_types/rename_collection_gen.h"
#include "mongo/util/string_map.h"

namespace mongo {

class ScopedRenameCollection;

/**
 * Thread-safe object that keeps track of any active renameCollection commands running. There is
 * only one instance of this object per shard.
 */
class ActiveRenameCollectionRegistry {
    ActiveRenameCollectionRegistry(const ActiveRenameCollectionRegistry&) = delete;
    ActiveRenameCollectionRegistry& operator=(const ActiveRenameCollectionRegistry&) = delete;

public:
    ActiveRenameCollectionRegistry();
    ~ActiveRenameCollectionRegistry();

    static ActiveRenameCollectionRegistry& get(ServiceContext* service);
    static ActiveRenameCollectionRegistry& get(OperationContext* opCtx);

    /**
     * If the collection being requested to be renamed is not already being renamed on this shard,
     * registers an active renameCollection with the specified arguments. Returns a
     * ScopedRenameCollection object, which must be signaled by the caller before it goes out of
     * scope.
     *
     * If this collection is already being renamed on this shard and it has the exact same
     * arguments, returns a ScopedRenameCollection. The ScopedRenameCollection can be used to join
     * the already running shard collection.
     *
     * Otherwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedRenameCollection> registerRenameCollection(
        const ShardsvrRenameCollection& request);

private:
    friend class ScopedRenameCollection;

    // Describes the state of a currently active renameCollection operation
    struct ActiveRenameCollectionState {
        ActiveRenameCollectionState(ShardsvrRenameCollection inRequest)
            : activeRequest(std::move(inRequest)) {}

        /**
         * Constructs an error status to return in the case of conflicting operations.
         */
        Status constructErrorStatus(const ShardsvrRenameCollection& request) const;

        // Exact arguments of the currently active operation
        ShardsvrRenameCollection activeRequest;

        /**
         * Promise that will either be set with an error or emplaced with nothing. Used to
         * follow an async/await paradigm when trying to join duplicate requests.
         */
        SharedPromise<void> _promise;
    };

    /**
     * Unregisters a previously registered namespace with an ongoing renameCollection. Must only be
     * called if a previous call to registerRenameCollection has succeeded.
     */
    void _clearRenameCollection(std::string nss);

    // Fulfills the promise and emplaces nothing on the promise if the status is OK or sets an
    // error on the promise if it is not.
    void _setEmptyOrError(std::string nss, Status status);

    // Protects the state below
    stdx::mutex _mutex;

    // Map containing any collections currently being renamed
    StringMap<std::shared_ptr<ActiveRenameCollectionState>> _activeRenameCollectionMap;
};

/**
 * Object of this class is returned from the registerRenameCollection call of the active rename
 * collection registry. It can exist in two modes - 'execute' and 'join'. See the comments for
 * registerRenameCollection method for more details.
 */
class ScopedRenameCollection {
    ScopedRenameCollection(const ScopedRenameCollection&) = delete;
    ScopedRenameCollection& operator=(const ScopedRenameCollection&) = delete;

public:
    ScopedRenameCollection(std::string nss,
                           ActiveRenameCollectionRegistry* registry,
                           bool shouldExecute,
                           SharedSemiFuture<void> uuidFuture);
    ~ScopedRenameCollection();

    ScopedRenameCollection(ScopedRenameCollection&&);
    ScopedRenameCollection& operator=(ScopedRenameCollection&&);

    /**
     * Returns true if the renameCollection object is in the 'execute' mode. This means that the
     * caller can execute the renameCollection command. The holder must execute the command and
     * get the status from the promise.
     */
    bool mustExecute() const {
        return _shouldExecute;
    }

    /**
     * Must only be called if the object is in the 'execute' mode when the renameCollection command
     * was invoked (the command immediately executed). Will either set error if status is not OK or
     * emplace an empty value into promise.
     */
    void emplaceStatus(Status status);

    /**
     * Must only be called if the object is in the 'join' mode. Gets a future for the collection.
     */
    SharedSemiFuture<void> awaitExecution();

private:
    // Namespace of collection being renamed
    std::string _nss;

    // Registry from which to unregister the rename. Not owned.
    ActiveRenameCollectionRegistry* _registry;

    /**
     * Whether the holder is the first in line for a newly started renameCollection (in which case
     * the destructor must unregister) or the caller is joining on an already-running
     * renameCollection (in which case the caller must block and wait for completion).
     */
    bool _shouldExecute;

    // Future that will be signaled at the end of renameCollection.
    SharedSemiFuture<void> _future;
};

}  // namespace mongo
