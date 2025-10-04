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

#include "mongo/base/status.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/future.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/string_map.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
    ScopedSetShardRole(const ScopedSetShardRole&) = delete;
    ScopedSetShardRole(ScopedSetShardRole&&);
    ~ScopedSetShardRole();

private:
    OperationContext* const _opCtx;

    NamespaceString _nss;

    boost::optional<ShardVersion> _shardVersion;
    boost::optional<DatabaseVersion> _databaseVersion;
};

// Stashes the shard role for the given namespace.
// DON'T USE unless you understand very well what you're doing.
class ScopedStashShardRole {
public:
    ScopedStashShardRole(OperationContext* opCtx, const NamespaceString& nss);

    ScopedStashShardRole(const ScopedSetShardRole&) = delete;
    ScopedStashShardRole(ScopedSetShardRole&&) = delete;

    ~ScopedStashShardRole();

private:
    OperationContext* _opCtx;
    NamespaceString _nss;
    boost::optional<ShardVersion> _stashedShardVersion;
    boost::optional<DatabaseVersion> _stashedDatabaseVersion;
};

/**
 * A decoration on OperationContext representing per-operation shard version metadata sent to mongod
 * from mongos as a command parameter.
 *
 * The metadata for a particular operation can be retrieved using the get() method.
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
     * Similar to 'isComingFromRouter()' but also considers '_treatAsFromRouter'. This should be
     * used when an operation intentionally skips setting shard versions but still wants to tell if
     * it's sent from a router.
     */
    static bool shouldBeTreatedAsFromRouter(OperationContext* opCtx);

    /**
     * NOTE: DO NOT ADD any new usages of this class without including someone from the Catalog and
     * Routing team on the code review. Using this object will allow creating a collection in the
     * local catalog without registering anything in the global catalog.
     *
     * Instantiating this object on the stack indicates to the storage execution subsystem that it
     * is allowed to create a collection with the specified namespace (or the equivalent bucket
     * namespace) in this context and that the caller will be responsible for notifying the shard
     * Sharding subsystem of the collection creation. Note that in most of cases the
     * CollectionShardingRuntime associated to that nss will be set as UNSHARDED. However, there are
     * some scenarios in which it is required to set is as UNKNOWN: that's the reason why the
     * constructor has the 'forceCSRAsUnknownAfterCollectionCreation' parameter. You can find more
     * information about how the CSR is modified in ShardServerOpObserver::onCreateCollection.
     */
    class ScopedAllowImplicitCollectionCreate_UNSAFE {
    public:
        /* Please read the comment associated to this class */
        ScopedAllowImplicitCollectionCreate_UNSAFE(
            OperationContext* opCtx,
            const NamespaceString& nss,
            bool forceCSRAsUnknownAfterCollectionCreation = false);
        ~ScopedAllowImplicitCollectionCreate_UNSAFE();

    private:
        OperationContext* const _opCtx;
        ReplicaSetDDLTracker::ScopedReplicaSetDDL _scopedReplicaSetDDL;
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
     * Returns the shard version (i.e. maximum chunk version) of a namespace being used by the
     * operation. Documents in chunks which did not belong on this shard at this shard version
     * will be filtered out.
     */
    boost::optional<ShardVersion> getShardVersion(const NamespaceString& nss);

    /**
     * If 'db' matches the 'db' in the namespace the client sent versions for, returns the database
     * version sent by the client (if any), else returns boost::none.
     */
    boost::optional<DatabaseVersion> getDbVersion(const DatabaseName& dbName) const;

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

    void setTreatAsFromRouter(bool treatAsFromRouter = true) {
        _treatAsFromRouter = treatAsFromRouter;
    }

    bool shouldSkipDirectConnectionChecks() {
        return _shouldSkipDirectConnectionChecks;
    }

    void setShouldSkipDirectShardConnectionChecks(bool skipDirectConnectionChecks = true) {
        _shouldSkipDirectConnectionChecks = skipDirectConnectionChecks;
    }

    void setBypassCheckAllShardRoleAcquisitionsVersioned(bool set) {
        _bypassCheckAllShardRoleAcquisitionsVersioned = set;
    }

    bool getBypassCheckAllShardRoleAcquisitionsVersioned() const {
        return _bypassCheckAllShardRoleAcquisitionsVersioned;
    }

private:
    friend class ScopedSetShardRole;
    friend class ScopedStashShardRole;
    friend class ShardServerOpObserver;  // For access to _allowCollectionCreation below

    // Stores information about allowed implicit collection creation
    struct ImplicitCreationInfo {
        // Specifies which namesace implicit creation is allowed on. If none, then implicit creation
        // if not allowed.
        boost::optional<NamespaceString> _creationNss{boost::none};
        // Specifies whether the CollectionShardingRuntime should be set as unknown after collection
        // creation
        bool _forceCSRAsUnknownAfterCollectionCreation{false};
        // Used to track multiple creations of the scoped object to know when to disallow creation.
        int recursion{0};
    };

    MONGO_MOD_NEEDS_REPLACEMENT ImplicitCreationInfo _implicitCreationInfo;


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
    stdx::unordered_map<DatabaseName, DatabaseVersionTracker> _databaseVersions;

    // This value can only be set when a rerouting exception occurs during a write operation, and
    // must be handled before this object gets destructed.
    boost::optional<Status> _shardingOperationFailedStatus;

    // Set when the operation comes from a router but intentionally skips setting the database or
    // the shard version.
    bool _treatAsFromRouter{false};

    // Set when an operation wishes to entirely skip direct shard connection checks. This should be
    // false in almost all situations.
    bool _shouldSkipDirectConnectionChecks{false};

    // Set to request ShardRole acquisitions to not assert that all operations originating from a
    // router perform all their ShardRole collection acquisitions with an explicit shard version.
    bool _bypassCheckAllShardRoleAcquisitionsVersioned{false};
};

class BypassCheckAllShardRoleAcquisitionsVersioned {
public:
    BypassCheckAllShardRoleAcquisitionsVersioned(OperationContext* opCtx) : _opCtx(opCtx) {
        OperationShardingState::get(opCtx).setBypassCheckAllShardRoleAcquisitionsVersioned(true);
    }
    ~BypassCheckAllShardRoleAcquisitionsVersioned() {
        OperationShardingState::get(_opCtx).setBypassCheckAllShardRoleAcquisitionsVersioned(false);
    }
    BypassCheckAllShardRoleAcquisitionsVersioned(
        const BypassCheckAllShardRoleAcquisitionsVersioned&) = delete;
    BypassCheckAllShardRoleAcquisitionsVersioned(BypassCheckAllShardRoleAcquisitionsVersioned&&) =
        delete;

private:
    OperationContext* const _opCtx;
};

}  // namespace mongo
