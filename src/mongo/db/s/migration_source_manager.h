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

#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_coordinator.h"
#include "mongo/db/s/move_timing_helper.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/util/timer.h"

namespace mongo {

struct ShardingStatistics;

/**
 * The donor-side migration state machine. This object must be created and owned by a single thread,
 * which controls its lifetime and should not be passed across threads. Unless explicitly indicated
 * its methods must not be called from more than one thread and must not be called while any locks
 * are held.
 *
 * The intended workflow is as follows:
 *  - Acquire a distributed lock on the collection whose chunk is about to be moved.
 *  - Instantiate a MigrationSourceManager on the stack. This will snapshot the latest collection
 *      metadata, which should stay stable because of the distributed collection lock.
 *  - Call startClone to initiate background cloning of the chunk contents. This will perform the
 *      necessary registration of the cloner with the replication subsystem and will start listening
 *      for document changes, while at the same time responding to data fetch requests from the
 *      recipient.
 *  - Call awaitUntilCriticalSectionIsAppropriate to wait for the cloning process to catch up
 *      sufficiently so we don't keep the server in read-only state for too long.
 *  - Call enterCriticalSection to cause the shard to enter in 'read only' mode while the latest
 *      changes are drained by the recipient shard.
 *  - Call commitDonateChunk to commit the chunk move in the config server's metadata and leave the
 *      read only (critical section) mode.
 *
 * At any point in time it is safe to let the MigrationSourceManager object go out of scope in which
 * case the desctructor will take care of clean up based on how far we have advanced. One exception
 * is the commitDonateChunk and its comments explain the reasoning.
 */
class MigrationSourceManager {
    MigrationSourceManager(const MigrationSourceManager&) = delete;
    MigrationSourceManager& operator=(const MigrationSourceManager&) = delete;

public:
    /**
     * Retrieves the MigrationSourceManager pointer that corresponds to the given collection under
     * a CollectionShardingRuntime that has its ResourceMutex locked.
     */
    static MigrationSourceManager* get(CollectionShardingRuntime* csr,
                                       CollectionShardingRuntime::CSRLock& csrLock);

    /**
     * If the currently installed migration has reached the cloning stage (i.e., after startClone),
     * returns the cloner currently in use.
     *
     * Must be called with a both a collection lock and the CSRLock.
     */
    static std::shared_ptr<MigrationChunkClonerSource> getCurrentCloner(
        CollectionShardingRuntime* csr, CollectionShardingRuntime::CSRLock& csrLock);

    /**
     * Instantiates a new migration source manager with the specified migration parameters. Must be
     * called with the distributed lock acquired in advance (not asserted).
     *
     * Loads the most up-to-date collection metadata and uses it as a starting point. It is assumed
     * that because of the distributed lock, the collection's metadata will not change further.
     *
     * May throw any exception. Known exceptions are:
     *  - InvalidOptions if the operation context is missing shard version
     *  - StaleConfigException if the expected collection version does not match what we find it
     *      to be after acquiring the distributed lock.
     */
    MigrationSourceManager(OperationContext* opCtx,
                           ShardsvrMoveRange&& request,
                           WriteConcernOptions&& writeConcern,
                           ConnectionString donorConnStr,
                           HostAndPort recipientHost);
    ~MigrationSourceManager();

    /**
     * Contacts the donor shard and tells it to start cloning the specified chunk. This method will
     * fail if for any reason the donor shard fails to initiate the cloning sequence.
     *
     * Expected state: kCreated
     * Resulting state: kCloning on success, kDone on failure
     */
    void startClone();

    /**
     * Waits for the cloning to catch up sufficiently so we won't have to stay in the critical
     * section for a long period of time. This method will fail if any error occurs while the
     * recipient is catching up.
     *
     * Expected state: kCloning
     * Resulting state: kCloneCaughtUp on success, kDone on failure
     */
    void awaitToCatchUp();

    /**
     * Waits for the active clone operation to catch up and enters critical section. Once this call
     * returns successfully, no writes will be happening on this shard until the chunk donation is
     * committed. Therefore, commitChunkOnRecipient/commitChunkMetadata must be called as soon as
     * possible afterwards.
     *
     * Expected state: kCloneCaughtUp
     * Resulting state: kCriticalSection on success, kDone on failure
     */
    void enterCriticalSection();

    /**
     * Tells the recipient of the chunk to commit the chunk contents, which it received.
     *
     * Expected state: kCriticalSection
     * Resulting state: kCloneCompleted on success, kDone on failure
     */
    void commitChunkOnRecipient();

    /**
     * Tells the recipient shard to fetch the latest portion of data from the donor and to commit it
     * locally. After that it persists the changed metadata to the config servers and leaves the
     * critical section.
     *
     * NOTE: Since we cannot recover from failures to write chunk metadata to the config servers, if
     *       applying the committed chunk information fails and we cannot definitely verify that the
     *       write was definitely applied or not, this call may crash the server.
     *
     * Expected state: kCloneCompleted
     * Resulting state: kDone
     */
    void commitChunkMetadataOnConfig();

    /**
     * Aborts the migration after observing a concurrent index operation by marking its operation
     * context as killed.
     */
    SharedSemiFuture<void> abort();

    /**
     * Returns a report on the active migration.
     *
     * Must be called with some form of lock on the collection namespace.
     */
    BSONObj getMigrationStatusReport() const;

    const NamespaceString& nss() {
        return _args.getCommandParameter();
    }

private:
    // Used to track the current state of the source manager. See the methods above, which have
    // comments explaining the various state transitions.
    enum State {
        kCreated,
        kCloning,
        kCloneCaughtUp,
        kCriticalSection,
        kCloneCompleted,
        kCommittingOnConfig,
        kDone
    };

    CollectionMetadata _getCurrentMetadataAndCheckEpoch();

    /**
     * Called when any of the states fails. May only be called once and will put the migration
     * manager into the kDone state.
     */
    void _cleanup(bool completeMigration) noexcept;

    /**
     * May be called at any time. Unregisters the migration source manager from the collection,
     * restores the committed metadata (if in critical section) and logs error in the change log to
     * indicate that the migration has failed.
     *
     * Expected state: Any
     * Resulting state: kDone
     */
    void _cleanupOnError() noexcept;

    // This is the opCtx of the moveChunk request that constructed the MigrationSourceManager.
    // The caller must guarantee it outlives the MigrationSourceManager.
    OperationContext* const _opCtx;

    // The parameters to the moveRange command
    ShardsvrMoveRange _args;

    // The write concern received for the moveRange command
    const WriteConcernOptions _writeConcern;

    // The resolved connection string of the donor shard
    const ConnectionString _donorConnStr;

    // The resolved primary of the recipient shard
    const HostAndPort _recipientHost;

    // Stores a reference to the process sharding statistics object which needs to be updated
    ShardingStatistics& _stats;

    // Information about the moveChunk to be used in the critical section.
    const BSONObj _critSecReason;

    // Times the entire moveChunk operation
    const Timer _entireOpTimer;

    // Utility for constructing detailed logs for the steps of the chunk migration
    MoveTimingHelper _moveTimingHelper;

    // Promise which will be signaled when the migration source manager has finished running and is
    // ready to be destroyed
    SharedPromise<void> _completion;

    // Starts counting from creation time and is used to time various parts from the lifetime of the
    // move chunk sequence
    Timer _cloneAndCommitTimer;

    // The current state. Used only for diagnostics and validation.
    State _state{kCreated};

    // Responsible for registering and unregistering the MigrationSourceManager from the collection
    // sharding runtime for the collection
    class ScopedRegisterer {
    public:
        ScopedRegisterer(MigrationSourceManager* msm,
                         CollectionShardingRuntime* csr,
                         const CollectionShardingRuntime::CSRLock& csrLock);
        ~ScopedRegisterer();

    private:
        MigrationSourceManager* const _msm;
    };
    boost::optional<ScopedRegisterer> _scopedRegisterer;

    // The epoch of the collection being migrated and its UUID, as of the time the migration
    // started. Values are boost::optional only up until the constructor runs, because UUID doesn't
    // have a default constructor.
    boost::optional<OID> _collectionEpoch;
    boost::optional<UUID> _collectionUUID;

    // The version of the chunk at the time the migration started.
    boost::optional<ChunkVersion> _chunkVersion;

    // The chunk cloner source. Only available if there is an active migration going on. To set and
    // remove it, a collection lock and the CSRLock need to be acquired first in order to block all
    // logOp calls and then the mutex. To access it, only the mutex is necessary. Available after
    // cloning stage has completed.
    std::shared_ptr<MigrationChunkClonerSource> _cloneDriver;

    // Contains logic for ensuring the donor's and recipient's config.rangeDeletions entries are
    // correctly updated based on whether the migration committed or aborted.
    boost::optional<migrationutil::MigrationCoordinator> _coordinator;

    // Holds the in-memory critical section for the collection. Only set when migration has reached
    // the critical section phase.
    boost::optional<CollectionCriticalSection> _critSec;

    // The statistics about a chunk migration to be included in moveChunk.commit
    boost::optional<BSONObj> _recipientCloneCounts;

    // Optional future that is populated if the migration succeeds and range deletion is scheduled
    // on this node. The future is set when the range deletion completes. Used if the moveChunk was
    // sent with waitForDelete.
    boost::optional<SemiFuture<void>> _cleanupCompleteFuture;
};

}  // namespace mongo
