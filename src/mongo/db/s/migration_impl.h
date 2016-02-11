/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/forwarding_catalog_manager.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

class CollectionMetadata;
class MigrationSessionId;
class OperationContext;
template <typename T>
class StatusWith;

/**
 * Contains all the runtime state for an active move operation and allows persistence of this state
 * to BSON, so it can be resumed. Intended to be used from within a single thread of execution
 * (single OperationContext) at a time and should not be moved between threads.
 */
class ChunkMoveOperationState {
    MONGO_DISALLOW_COPYING(ChunkMoveOperationState);

public:
    ChunkMoveOperationState(OperationContext* txn, NamespaceString ns);
    ~ChunkMoveOperationState();

    /**
     * Extracts and validates the move chunk parameters from the given cmdObj.
     */
    Status initialize(const BSONObj& cmdObj);

    /**
     * Acquires the distributed lock for the collection, whose chunk is being moved and fetches the
     * latest metadata as of the time of the call. The fetched metadata will be cached on the
     * operation state until the entire operation completes. Also, because of the distributed lock
     * being held, other processes should not change it on the config servers.
     *
     * Returns a pointer to the distributed lock acquired by the operation so it can be periodically
     * checked for liveness. The returned value is owned by the move operation state and should not
     * be accessed after it completes.
     *
     * TODO: Once the entire chunk move process is moved to be inside this state machine, there
     *       will not be any need to expose the distributed lock.
     */
    StatusWith<ForwardingCatalogManager::ScopedDistLock*> acquireMoveMetadata();

    /**
     * Starts the move chunk operation.
     */
    Status start(const MigrationSessionId& sessionId, const BSONObj& shardKeyPattern);

    /**
     * Implements the migration critical section. Needs to be invoked after all data has been moved
     * after which it will enter the migration critical section, move the remaining batch and then
     * donate the chunk.
     *
     * Returns OK if the migration commits successfully or a status describing the error otherwise.
     * Since some migration failures are non-recoverable, it may also shut down the server on
     * certain errors.
     */
    Status commitMigration(const MigrationSessionId& sessionId);

    const NamespaceString& getNss() const {
        return _nss;
    }

    const std::string& getFromShard() const {
        return _fromShard;
    }

    const ConnectionString& getFromShardCS() const {
        return _fromShardCS;
    }

    const std::string& getToShard() const {
        return _toShard;
    }

    const ConnectionString& getToShardCS() const {
        return _toShardCS;
    }

    const BSONObj& getMinKey() const {
        return _minKey;
    }

    const BSONObj& getMaxKey() const {
        return _maxKey;
    }

    /**
     * Retrieves the highest chunk version on this shard of the collection being moved as of the
     * time the distributed lock was acquired. It is illegal to call this method before
     * acquireMoveMetadata has been called and succeeded.
     */
    ChunkVersion getShardVersion() const;

    /**
     * Retrieves the snapshotted collection metadata as of the time the distributed lock was
     * acquired. It is illegal to call this method before acquireMoveMetadata has been called and
     * succeeded.
     */
    std::shared_ptr<CollectionMetadata> getCollMetadata() const;

private:
    // The context of which the migration is running on.
    OperationContext* const _txn = nullptr;
    const NamespaceString _nss;

    // The source and recipient shard ids
    std::string _fromShard;
    std::string _toShard;

    // Resolved shard connection strings for the above shards
    ConnectionString _fromShardCS;
    ConnectionString _toShardCS;

    // ChunkVersion for the collection sent along with the command
    ChunkVersion _collectionVersion;

    // Min and max key of the chunk being moved
    BSONObj _minKey;
    BSONObj _maxKey;

    // The distributed lock, which protects other migrations from happening on the same collection
    boost::optional<StatusWith<ForwardingCatalogManager::ScopedDistLock>> _distLockStatus;

    // The cached collection metadata and the shard version from the time the migration process
    // started. This metadata is guaranteed to not change until either failure or successful
    // completion, because the distributed lock is being held.
    ChunkVersion _shardVersion;
    std::shared_ptr<CollectionMetadata> _collMetadata;

    // True if this migration is running.
    bool _isRunning = false;
};

}  // namespace mongo
