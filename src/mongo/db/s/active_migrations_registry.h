/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

class OperationContext;
class ScopedRegisterDonateChunk;
class ScopedRegisterReceiveChunk;
template <typename T>
class StatusWith;

/**
 * Thread-safe object, which keeps track of the active migrations running on a node and limits them
 * to only one per-shard. There is only one instance of this object per shard.
 */
class ActiveMigrationsRegistry {
    MONGO_DISALLOW_COPYING(ActiveMigrationsRegistry);

public:
    ActiveMigrationsRegistry();
    ~ActiveMigrationsRegistry();

    /**
     * If there are no migrations running on this shard, registers an active migration with the
     * specified arguments and returns a ScopedRegisterDonateChunk, which must be signaled by the
     * caller before it goes out of scope.
     *
     * If there is an active migration already running on this shard and it has the exact same
     * arguments, returns a ScopedRegisterDonateChunk, which can be used to join the already running
     * migration.
     *
     * Otherwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedRegisterDonateChunk> registerDonateChunk(const MoveChunkRequest& args);

    /**
     * If there are no migrations running on this shard, registers an active receive operation with
     * the specified session id and returns a ScopedRegisterReceiveChunk, which will unregister it
     * when it goes out of scope.
     *
     * Otherwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedRegisterReceiveChunk> registerReceiveChunk(const NamespaceString& nss,
                                                                const ChunkRange& chunkRange,
                                                                const ShardId& fromShardId);

    /**
     * If a migration has been previously registered through a call to registerDonateChunk returns
     * that namespace. Otherwise returns boost::none.
     */
    boost::optional<NamespaceString> getActiveDonateChunkNss();

    /**
     * Returns a report on the active migration if there currently is one. Otherwise, returns an
     * empty BSONObj.
     *
     * Takes an IS lock on the namespace of the active migration, if one is active.
     */
    BSONObj getActiveMigrationStatusReport(OperationContext* opCtx);

private:
    friend class ScopedRegisterDonateChunk;
    friend class ScopedRegisterReceiveChunk;

    // Describes the state of a currently active moveChunk operation
    struct ActiveMoveChunkState {
        ActiveMoveChunkState(MoveChunkRequest inArgs)
            : args(std::move(inArgs)), notification(std::make_shared<Notification<Status>>()) {}

        /**
         * Constructs an error status to return in the case of conflicting operations.
         */
        Status constructErrorStatus() const;

        // Exact arguments of the currently active operation
        MoveChunkRequest args;

        // Notification event, which will be signaled when the currently active operation completes
        std::shared_ptr<Notification<Status>> notification;
    };

    // Describes the state of a currently active receive chunk operation
    struct ActiveReceiveChunkState {
        ActiveReceiveChunkState(NamespaceString inNss, ChunkRange inRange, ShardId inFromShardId)
            : nss(std::move(inNss)), range(std::move(inRange)), fromShardId(inFromShardId) {}

        /**
         * Constructs an error status to return in the case of conflicting operations.
         */
        Status constructErrorStatus() const;

        // Namesspace for which a chunk is being received
        NamespaceString nss;

        // Bounds of the chunk being migrated
        ChunkRange range;

        // Id of the shard from which the chunk is being received
        ShardId fromShardId;
    };

    /**
     * Unregisters a previously registered namespace with ongoing migration. Must only be called if
     * a previous call to registerDonateChunk has succeeded.
     */
    void _clearDonateChunk();

    /**
     * Unregisters a previously registered incoming migration. Must only be called if a previous
     * call to registerReceiveChunk has succeeded.
     */
    void _clearReceiveChunk();

    // Protects the state below
    stdx::mutex _mutex;

    // If there is an active moveChunk operation going on, this field contains the request, which
    // initiated it
    boost::optional<ActiveMoveChunkState> _activeMoveChunkState;

    // If there is an active receive of a chunk going on, this field contains the session id, which
    // initiated it
    boost::optional<ActiveReceiveChunkState> _activeReceiveChunkState;
};

/**
 * Object of this class is returned from the registerDonateChunk call of the active migrations
 * registry. It can exist in two modes - 'unregister' and 'join'. See the comments for
 * registerDonateChunk method for more details.
 */
class ScopedRegisterDonateChunk {
    MONGO_DISALLOW_COPYING(ScopedRegisterDonateChunk);

public:
    ScopedRegisterDonateChunk(ActiveMigrationsRegistry* registry,
                              bool forUnregister,
                              std::shared_ptr<Notification<Status>> completionNotification);
    ~ScopedRegisterDonateChunk();

    ScopedRegisterDonateChunk(ScopedRegisterDonateChunk&&);
    ScopedRegisterDonateChunk& operator=(ScopedRegisterDonateChunk&&);

    /**
     * Returns true if the migration object is in the 'unregister' mode, which means that the holder
     * must execute the moveChunk command and complete with a status.
     */
    bool mustExecute() const {
        return _forUnregister;
    }

    /**
     * Must only be called if the object is in the 'unregister' mode. Signals any callers, which
     * might be blocked in waitForCompletion.
     */
    void complete(Status status);

    /**
     * Must only be called if the object is in the 'join' mode. Blocks until the main executor of
     * the moveChunk command calls complete.
     */
    Status waitForCompletion(OperationContext* opCtx);

private:
    // Registry from which to unregister the migration. Not owned.
    ActiveMigrationsRegistry* _registry;

    // Whether this is a newly started migration (in which case the destructor must unregister) or
    // joining an existing one (in which case the caller must wait for completion).
    bool _forUnregister;

    // This is the future, which will be signaled at the end of a migration
    std::shared_ptr<Notification<Status>> _completionNotification;
};

/**
 * Object of this class is returned from the registerReceiveChunk call of the active migrations
 * registry.
 */
class ScopedRegisterReceiveChunk {
    MONGO_DISALLOW_COPYING(ScopedRegisterReceiveChunk);

public:
    ScopedRegisterReceiveChunk(ActiveMigrationsRegistry* registry);
    ~ScopedRegisterReceiveChunk();

    ScopedRegisterReceiveChunk(ScopedRegisterReceiveChunk&&);
    ScopedRegisterReceiveChunk& operator=(ScopedRegisterReceiveChunk&&);

private:
    // Registry from which to unregister the migration. Not owned.
    ActiveMigrationsRegistry* _registry;
};

}  // namespace mongo
