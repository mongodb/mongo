/*
*    Copyright (C) 2015 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/namespace_string.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version_gen.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

class OperationContext;

/**
 * A decoration on OperationContext tracking per-operation sharding state, such as the client's
 * sent shardVersion, databaseVersion, and allowImplicitCollectionCreation fields, and whether this
 * operation should wait for a migration critical section to complete before returning.
 */
class OperationShardingState {
    MONGO_DISALLOW_COPYING(OperationShardingState);

public:
    OperationShardingState();

    /**
     * Retrieves a reference to the shard version decorating the OperationContext, 'opCtx'.
     */
    static OperationShardingState& get(OperationContext* opCtx);

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
     * Returns whether a shardVersion for 'nss' was sent by the client.
     */
    bool hasClientShardVersion(const NamespaceString& nss) const;

    /**
     * Returns whether a shardVersion for any namespace was sent by the client.
     */
    bool hasClientShardVersionForAnyNamespace() const;

    /**
     * Returns whether a database version for 'db' was sent by the client.
     */
    bool hasClientDbVersion(const std::string& db) const;

    /**
     * Returns whether a database version for any database was sent by the client.
     */
    bool hasClientDbVersionForAnyDb() const;

    /**
     * Inspects 'cmdObj' for shardVersion and databaseVersion fields, and sets them as the client's
     * shardVersion and dbVersion for 'nss'.
     */
    void setClientRoutingVersions(NamespaceString nss, BSONObj cmdObj);

    /**
     * Returns the client's sent shardVersion for 'nss', if one was sent, otherwise returns the
     * UNSHARDED version. This is for two historical reasons:
     *
     * 1) Some commands operate on multiple namespaces (such as agg with $out, agg with $lookup, and
     *    eval), but it's not possible to send a shardVersion for more than one namespace.
     *    Luckily, the secondary namespaces in all such commands are required to be unsharded. So,
     *    we return UNSHARDED for any namespace besides the (primary) namespace the client sent an
     *    explicit shardVersion for.
     * 2) Not all paths on mongos attached a shardVersion when targeting an unsharded collection.
     *
     * Operations that need to completely opt out of shardVersion checks set the IGNORED version.
     */
    ChunkVersion getClientShardVersion(const NamespaceString& nss) const;

    /**
     * Returns the client's sent dbVersion for 'db', if one was sent, otherwise returns boost::none.
     */
    boost::optional<DatabaseVersion> getClientDbVersion(const std::string& db) const;

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

private:
    // Specifies whether the request is allowed to create database/collection implicitly
    bool _allowImplicitCollectionCreation{true};

    boost::optional<NamespaceString> _nss;
    boost::optional<ChunkVersion> _shardVersion;
    boost::optional<DatabaseVersion> _dbVersion;

    // This value will only be non-null if version check during the operation execution failed due
    // to stale version and there was a migration for that namespace, which was in critical section.
    std::shared_ptr<Notification<void>> _migrationCriticalSectionSignal;
};

}  // namespace mongo
