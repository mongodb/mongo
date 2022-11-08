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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/timer.h"

namespace mongo {

class OperationContext;
class Shard;
struct ShardingStatistics;
class Status;

/**
 * The donor-side movePrimary state machine. This object must be created and owned by a single
 * thread, which controls its lifetime and should not be passed across threads. Unless explicitly
 * indicated its methods must not be called from more than one thread and must not be called while
 * any locks are held.
 *
 * The intended workflow is as follows:
 *  - Acquire a distributed lock on the database whose primary is about to be moved.
 *  - Instantiate a MovePrimarySourceManager on the stack.
 *  - Call clone to start and finish cloning of the unsharded collections.
 *  - Call enterCriticalSection to cause the shard to enter in 'read only' mode while the config
 *      server is notified of the new primary.
 *  - Call commitOnConfig to indicate the new primary in the config server metadata.
 *  - Call cleanStaleData to drop now-unused collections (and potentially databases) on the
 *      old primary.
 *
 * At any point in time it is safe to let the MovePrimarySourceManager object go out of scope in
 * which case the destructor will take care of clean up based on how far we have advanced.
 */
class MovePrimarySourceManager {
    MovePrimarySourceManager(const MovePrimarySourceManager&) = delete;
    MovePrimarySourceManager& operator=(const MovePrimarySourceManager&) = delete;

public:
    /**
     * Instantiates a new movePrimary source manager. Must be called with the distributed lock
     * acquired in advance (not asserted).
     *
     * May throw any exception. Known exceptions (TODO) are:
     *  - InvalidOptions if the operation context is missing database version
     *  - StaleConfigException if the expected database version does not match what we find it
     *      to be after acquiring the distributed lock.
     */

    MovePrimarySourceManager(OperationContext* opCtx,
                             ShardMovePrimary requestArgs,
                             StringData dbname,
                             ShardId& fromShard,
                             ShardId& toShard);
    ~MovePrimarySourceManager();

    /**
     * Returns the namespace for which this source manager is active.
     */
    NamespaceString getNss() const;

    /**
     * Contacts the recipient shard and tells it to start cloning the specified chunk. This method
     * will fail if for any reason the recipient shard fails to complete the cloning sequence.
     *
     * Expected state: kCreated
     * Resulting state: kCloning on success, kDone on failure
     */
    Status clone(OperationContext* opCtx);

    /**
     * Once this call returns successfully, no writes will be happening on this shard until the
     * movePrimary is committed. Therefore, commitMovePrimaryMetadata must be called as soon as
     * possible afterwards.
     *
     * Expected state: kCloneCaughtUp
     * Resulting state: kCriticalSection on success, kDone on failure
     */
    Status enterCriticalSection(OperationContext* opCtx);

    /**
     * Persists the updated DatabaseVersion on the config server and leaves the critical section.
     *
     * Expected state: kCriticalSection
     * Resulting state: kNeedCleanStaleData
     */
    Status commitOnConfig(OperationContext* opCtx);

    /**
     * Clears stale collections (and potentially databases) on the old primary.
     *
     * Expected state: kNeedCleanStaleData
     * Resulting state: kDone
     */
    Status cleanStaleData(OperationContext* opCtx);

    /**
     * May be called at any time. Unregisters the movePrimary source manager from the database and
     * logs an error in the change log to indicate that the migration has failed.
     *
     * Expected state: Any
     * Resulting state: kDone
     */
    void cleanupOnError(OperationContext* opCtx);

private:
    static BSONObj _buildMoveLogEntry(const std::string& db,
                                      const std::string& from,
                                      const std::string& to) {
        BSONObjBuilder details;
        details.append("database", db);
        details.append("from", from);
        details.append("to", to);

        return details.obj();
    }

    /**
     * Invokes the _configsvrCommitMovePrimary command of the config server to reassign the primary
     * shard of the database.
     */
    Status _commitOnConfig(OperationContext* opCtx, const DatabaseVersion& expectedDbVersion);

    /**
     * Updates the config server's metadata in config.databases collection to reassign the primary
     * shard of the database.
     *
     * This logic is not synchronized with the removeShard command and simultaneous invocations of
     * movePrimary and removeShard can lead to data loss.
     */
    Status _fallbackCommitOnConfig(OperationContext* opCtx,
                                   const DatabaseVersion& expectedDbVersion);

    // Used to track the current state of the source manager. See the methods above, which have
    // comments explaining the various state transitions.
    enum State {
        kCreated,
        kCloning,
        kCloneCaughtUp,
        kCriticalSection,
        kCloneCompleted,
        kNeedCleanStaleData,
        kDone
    };

    /**
     * Called when any of the states fails. May only be called once and will put the migration
     * manager into the kDone state.
     */
    void _cleanup(OperationContext* opCtx);

    // The parameters to the movePrimary command
    const ShardMovePrimary _requestArgs;

    // The database whose primary we are moving.
    const StringData _dbname;

    // The donor shard
    const ShardId& _fromShard;

    // The recipient shard
    const ShardId& _toShard;

    // Collections that were cloned to the new primary
    std::vector<NamespaceString> _clonedColls;

    // Indicates whether sharded collections exist on the old primary.
    bool _shardedCollectionsExistOnDb;

    // Times the entire movePrimary operation
    const Timer _entireOpTimer;

    // The current state. Used only for diagnostics and validation.
    State _state{kCreated};

    // Information about the movePrimary to be used in the critical section.
    const BSONObj _critSecReason;
};

}  // namespace mongo
