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
#include "mongo/util/concurrency/notification.h"

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
     * Parses shard version from the command parameters 'cmdObj' and stores the results in this
     * object along with the give namespace that is associated with the version. Does nothing
     * if no shard version is attached to the command.
     *
     * Expects the format { ..., shardVersion: [<version>, <epoch>] }.
     *
     * This initialization may only be performed once for the lifetime of the object, which
     * coincides with the lifetime of the request.
     */
    void initializeShardVersion(NamespaceString nss, const BSONElement& shardVersionElement);

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
     * Stores the given chunk version of a namespace into this object.
     */
    void setShardVersion(NamespaceString nss, ChunkVersion newVersion);

    /**
     * Undoes setting the shard version for the given namespace. Needed for views.
     */
    void unsetShardVersion(NamespaceString nss);

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
    /**
     * Resets this object back as if it was default constructed (ie _hasVersion is false,
     * _shardVersion is UNSHARDED, _ns is empty).
     */
    void _clear();

    // Specifies whether the request is allowed to create database/collection implicitly
    bool _allowImplicitCollectionCreation{true};

    bool _hasVersion = false;
    ChunkVersion _shardVersion{ChunkVersion::UNSHARDED()};
    NamespaceString _ns;

    // This value will only be non-null if version check during the operation execution failed due
    // to stale version and there was a migration for that namespace, which was in critical section.
    std::shared_ptr<Notification<void>> _migrationCriticalSectionSignal;
};

}  // namespace mongo
