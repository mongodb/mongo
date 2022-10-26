/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/uuid.h"

#include <set>

namespace mongo {

/**
 * Registry to allow only one type of active serverless operation at a time. It allows multiple
 * simultaneous operations of the same type.
 */
class ServerlessOperationLockRegistry {
    ServerlessOperationLockRegistry(const ServerlessOperationLockRegistry&) = delete;
    ServerlessOperationLockRegistry& operator=(const ServerlessOperationLockRegistry&) = delete;

public:
    ServerlessOperationLockRegistry() = default;

    static const ServiceContext::Decoration<ServerlessOperationLockRegistry> get;

    enum LockType { kShardSplit, kTenantDonor, kTenantRecipient };

    /**
     * Acquire the serverless lock for LockType and adds operationId to the set of
     * instances tracked. Throws ConflictingServerlessOperation error if there is already an
     * activeServerlessOperation in progress with a different namespace than operationNamespace.
     */
    void acquireLock(LockType lockType, const UUID& operationId);

    /**
     * If _activeOpSeverlessOperation matches LockType, removes the given operationId from
     * the set of active instances and releases the lock if the set becomes empty. Invariant if
     * lockType or operationId does not own the lock.
     */
    void releaseLock(LockType lockType, const UUID& operationId);

    /**
     * Called when a state document collection is dropped. If the collection's lockType currently
     * holds the lock, it releases the lock. If it does not own the lock, the function does nothing.
     */
    void onDropStateCollection(LockType lockType);

    void clear();

    /**
     * Scan serverless state documents and acquire the serverless mutual exclusion lock if needed.
     */
    static void recoverLocks(OperationContext* opCtx);

    /**
     * Appends the exclusion status to the BSONObjBuilder.
     */
    void appendInfoForServerStatus(BSONObjBuilder* builder) const;

    boost::optional<ServerlessOperationLockRegistry::LockType> getActiveOperationType_forTest();

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ServerlessMutualExclusionRegistry::_mutex");
    boost::optional<LockType> _activeLockType;
    std::set<UUID> _activeOperations;
};

}  // namespace mongo
