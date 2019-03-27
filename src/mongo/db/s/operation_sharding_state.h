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
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version_gen.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/string_map.h"

namespace mongo {

class OperationContext;

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
     * Returns true if the the current operation was sent by the caller with shard version
     * information attached, meaning that it must perform shard version checking and orphan
     * filtering.
     */
    static bool isOperationVersioned(OperationContext* opCtx);

    /**
     * Requests on a sharded collection that are broadcast without a shardVersion should not cause
     * the collection to be created on a shard that does not know about the collection already,
     * since the collection options will not be propagated. Such requests specify to disallow
     * collection creation, which is saved here.
     */
    void setAllowImplicitCollectionCreation(const BSONElement& allowImplicitCollectionCreationElem);

    /**
     * Specifies whether the request is allowed to create database/collection implicitly.
     */
    bool allowImplicitCollectionCreation() const;

    /**
     * Parses shardVersion and databaseVersion from 'cmdObj' and stores the results in this object
     * along with the given namespace that is associated with the versions. Does nothing if no
     * shardVersion or databaseVersion is attached to the command.
     *
     * Expects 'cmdObj' to have format
     * { ...,
     *   shardVersion: [<version>, <epoch>],
     *   databaseVersion: { uuid: <UUID>, version: <int> },
     * ...}
     *
     * This initialization may only be performed once for the lifetime of the object, which
     * coincides with the lifetime of the client's request.
     */
    void initializeClientRoutingVersions(NamespaceString nss, const BSONObj& cmdObj);

    /**
     * Returns whether or not there is a shard version associated with this operation.
     */
    bool hasShardVersion() const;

    /**
     * Returns the shard version (i.e. maximum chunk version) of a namespace being used by the
     * operation. Documents in chunks which did not belong on this shard at this shard version
     * will be filtered out.
     *
     * Returns ChunkVersion::UNSHARDED() if this operation has no shard version information
     * for the requested namespace.
     */
    ChunkVersion getShardVersion(const NamespaceString& nss) const;

    /**
     * Returns true if the client sent a databaseVersion for any namespace.
     */
    bool hasDbVersion() const;

    /**
     * If 'db' matches the 'db' in the namespace the client sent versions for, returns the database
     * version sent by the client (if any), else returns boost::none.
     */
    boost::optional<DatabaseVersion> getDbVersion(const StringData dbName) const;

    /**
     * Makes the OperationShardingState behave as if an UNSHARDED shardVersion was sent for every
     * possible namespace.
     */
    void setGlobalUnshardedShardVersion();

    /**
     * This call is a no op if there isn't a currently active migration critical section. Otherwise
     * it will wait for the critical section to complete up to the remaining operation time.
     *
     * Returns true if the call actually waited because of migration critical section (regardless if
     * whether it timed out or not), false if there was no active migration critical section.
     */
    bool waitForMigrationCriticalSectionSignal(OperationContext* opCtx);

    /**
     * Setting this value indicates that when the version check failed, there was an active
     * migration for the namespace and that it would be prudent to wait for the critical section to
     * complete before retrying so the router doesn't make wasteful requests.
     */
    void setMigrationCriticalSectionSignal(std::shared_ptr<Notification<void>> critSecSignal);

    /**
     * This call is a no op if there isn't a currently active movePrimary critical section.
     * Otherwise it will wait for the critical section to complete up to the remaining operation
     * time.
     *
     * Returns true if the call actually waited because of movePrimary critical section (regardless
     * whether it timed out or not), false if there was no active movePrimary critical section.
     */
    bool waitForMovePrimaryCriticalSectionSignal(OperationContext* opCtx);

    /**
     * Setting this value indicates that when the version check failed, there was an active
     * movePrimary for the namespace and that it would be prudent to wait for the critical section
     * to complete before retrying so the router doesn't make wasteful requests.
     */
    void setMovePrimaryCriticalSectionSignal(std::shared_ptr<Notification<void>> critSecSignal);

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
    // Specifies whether the request is allowed to create database/collection implicitly
    bool _allowImplicitCollectionCreation{true};

    // Should be set to true if all collections accessed are expected to be unsharded.
    bool _globalUnshardedShardVersion = false;

    // The OperationShardingState class supports storing shardVersions for multiple namespaces (and
    // databaseVersions for multiple databases), even though client code has not been written yet to
    // *send* multiple shardVersions or databaseVersions.
    StringMap<ChunkVersion> _shardVersions;
    StringMap<DatabaseVersion> _databaseVersions;

    // This value will only be non-null if version check during the operation execution failed due
    // to stale version and there was a migration for that namespace, which was in critical section.
    std::shared_ptr<Notification<void>> _migrationCriticalSectionSignal;

    // This value will only be non-null if version check during the operation execution failed due
    // to stale version and there was a movePrimary for that namespace, which was in critical
    // section.
    std::shared_ptr<Notification<void>> _movePrimaryCriticalSectionSignal;

    // This value can only be set when a rerouting exception occurs during a write operation, and
    // must be handled before this object gets destructed.
    boost::optional<Status> _shardingOperationFailedStatus;
};

}  // namespace mongo
