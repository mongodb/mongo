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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/future.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * Marks the opCtx during scope in which it has been instantiated as running in the shard role for
 * the specified collection. This indicates to the underlying storage system that the caller has
 * performed 'routing', in the sense that it is aware of what data is located on this node.
 */
class ScopedSetShardRole {
public:
    ScopedSetShardRole(OperationContext* opCtx,
                       NamespaceString nss,
                       boost::optional<ShardVersion> shardVersion,
                       boost::optional<DatabaseVersion> databaseVersion);
    ~ScopedSetShardRole();

private:
    OperationContext* const _opCtx;

    NamespaceString _nss;

    boost::optional<ShardVersion> _shardVersion;
    boost::optional<DatabaseVersion> _databaseVersion;
};

/**
 * A decoration on OperationContext representing per-operation shard version metadata sent to mongod
 * from mongos as a command parameter.
 *
 * The metadata for a particular operation can be retrieved using the get() method.
 *
 * Note: This only supports storing the version for a single namespace.
 */
class OperationShardingState {
    OperationShardingState(const OperationShardingState&) = delete;
    OperationShardingState& operator=(const OperationShardingState&) = delete;

public:
    OperationShardingState();
    ~OperationShardingState();

    /**
     * Retrieves a reference to the shard version decorating the OperationContext, 'opCtx'.
     */
    static OperationShardingState& get(OperationContext* opCtx);

    /**
     * Returns true if the the current operation was sent from an upstream router, rather than it
     * being a direct connection against the shard. The way this decision is made is based on
     * whether there is shard version declared for any namespace.
     */
    static bool isComingFromRouter(OperationContext* opCtx);

    /**
     * NOTE: DO NOT ADD any new usages of this class without including someone from the Sharding
     * Team on the code review.
     *
     * Instantiating this object on the stack indicates to the storage execution subsystem that it
     * is allowed to create any collection in this context and that the caller will be responsible
     * for notifying the shard Sharding sybsystem of the collection creation.
     */
    class ScopedAllowImplicitCollectionCreate_UNSAFE {
    public:
        ScopedAllowImplicitCollectionCreate_UNSAFE(OperationContext* opCtx);
        ~ScopedAllowImplicitCollectionCreate_UNSAFE();

    private:
        OperationContext* const _opCtx;
    };

    /**
     * Same semantics as ScopedSetShardRole above, but the role remains set for the lifetime of the
     * opCtx.
     */
    static void setShardRole(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<ShardVersion>& shardVersion,
                             const boost::optional<DatabaseVersion>& dbVersion);

    /**
     * Used to clear the shard role from the opCtx for ddl operations which are not required to send
     * the index version (ex. split, merge). These operations will do their own metadata checks
     * rather than us the collection sharding runtime checks.
     */
    static void unsetShardRoleForLegacyDDLOperationsSentWithShardVersionIfNeeded(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Returns the shard version (i.e. maximum chunk version) of a namespace being used by the
     * operation. Documents in chunks which did not belong on this shard at this shard version
     * will be filtered out.
     */
    boost::optional<ShardVersion> getShardVersion(const NamespaceString& nss);

    /**
     * If 'db' matches the 'db' in the namespace the client sent versions for, returns the database
     * version sent by the client (if any), else returns boost::none.
     */
    boost::optional<DatabaseVersion> getDbVersion(StringData dbName) const;

    /**
     * This method implements a best-effort attempt to wait for the critical section to complete
     * before returning to the router at the previous step in order to prevent it from busy spinning
     * while the critical section is in progress.
     *
     * All waits for migration critical section should go through this code path, because it also
     * accounts for transactions and locking.
     */
    static Status waitForCriticalSectionToComplete(OperationContext* opCtx,
                                                   SharedSemiFuture<void> critSecSignal) noexcept;

    /**
     * Stores the failed status in _shardingOperationFailedStatus.
     *
     * This method may only be called once when a rerouting exception occurs. The caller
     * must process the status at exit.
     */
    void setShardingOperationFailedStatus(const Status& status);

    /**
     * Returns the failed status stored in _shardingOperationFailedStatus if any, and reset the
     * status to none.
     *
     * This method may only be called when the caller wants to process the status.
     */
    boost::optional<Status> resetShardingOperationFailedStatus();

private:
    friend class ScopedSetShardRole;
    friend class ShardServerOpObserver;  // For access to _allowCollectionCreation below

    // Specifies whether the request is allowed to create database/collection implicitly
    bool _allowCollectionCreation{false};

    // Stores the shard version expected for each collection that will be accessed
    struct ShardVersionTracker {
        ShardVersionTracker(ShardVersion v) : v(v) {}
        ShardVersionTracker(ShardVersionTracker&&) = default;
        ShardVersionTracker(const ShardVersionTracker&) = delete;
        ShardVersionTracker& operator=(const ShardVersionTracker&) = delete;
        ShardVersion v;
        int recursion{0};
    };
    StringMap<ShardVersionTracker> _shardVersions;

    // Stores the database version expected for each database that will be accessed
    struct DatabaseVersionTracker {
        DatabaseVersionTracker(DatabaseVersion v) : v(v) {}
        DatabaseVersionTracker(DatabaseVersionTracker&&) = default;
        DatabaseVersionTracker(const DatabaseVersionTracker&) = delete;
        DatabaseVersionTracker& operator=(const DatabaseVersionTracker&) = delete;
        DatabaseVersion v;
        int recursion{0};
    };
    StringMap<DatabaseVersionTracker> _databaseVersions;

    // This value can only be set when a rerouting exception occurs during a write operation, and
    // must be handled before this object gets destructed.
    boost::optional<Status> _shardingOperationFailedStatus;
};

}  // namespace mongo
