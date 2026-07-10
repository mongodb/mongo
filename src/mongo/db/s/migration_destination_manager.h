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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_batch_fetcher.h"
#include "mongo/db/s/migration_batch_inserter.h"
#include "mongo/db/s/migration_recipient_recovery_document_gen.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration_destination.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;

class StartChunkCloneRequest;
class Status;
struct WriteConcernOptions;

namespace repl {
class OpTime;
}

struct CollectionOptionsAndIndexes {
    UUID uuid;
    std::vector<BSONObj> indexSpecs;
    BSONObj idIndexSpec;
    BSONObj options;
};

/**
 * Drives the receiving side of the MongoD migration process. One instance exists per shard.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] MigrationDestinationManager
    : public ReplicaSetAwareServiceShardSvr<MigrationDestinationManager> {
    MigrationDestinationManager(const MigrationDestinationManager&) = delete;
    MigrationDestinationManager& operator=(const MigrationDestinationManager&) = delete;

public:
    enum State {
        kReady,
        kClone,
        kCatchup,
        kSteady,
        kCommitStart,
        kEnteredCritSec,
        kExitCritSec,
        kDone,
        kFail,
        kAbort
    };

    MigrationDestinationManager();
    ~MigrationDestinationManager() override;

    /**
     * Returns the singleton instance of the migration destination manager.
     */
    static MigrationDestinationManager* get(ServiceContext* serviceContext);
    static MigrationDestinationManager* get(OperationContext* opCtx);

    State getState() const;

    /**
     * Checks whether the MigrationDestinationManager is currently handling a migration.
     */
    bool isActive() const;
    bool isActiveOn(const NamespaceString& nss) const;

    /**
     * Reports the state of the migration manager as a BSON document.
     */
    void report(BSONObjBuilder& b, OperationContext* opCtx, bool waitForSteadyOrDone);

    /**
     * Returns a report on the active migration, if the migration is active. Otherwise return an
     * empty BSONObj.
     */
    BSONObj getMigrationStatusReport(
        const CollectionShardingRuntime::ScopedSharedCollectionShardingRuntime& scopedCsrLock);

    /**
     * Returns OK if migration started successfully. Requires a ScopedReceiveChunk, which guarantees
     * that there can only be one start() or restoreRecoveredMigrationState() call at any given
     * time.
     */
    Status start(OperationContext* opCtx,
                 const NamespaceString& nss,
                 ScopedReceiveChunk scopedReceiveChunk,
                 const StartChunkCloneRequest& cloneRequest,
                 const WriteConcernOptions& writeConcern);

    /**
     * Restores the MigrationDestinationManager state for a migration recovered on step-up. Requires
     * a ScopedReceiveChunk, which guarantees that there can only be one start() or
     * restoreRecoveredMigrationState() call at any given time.
     */
    Status restoreRecoveredMigrationState(OperationContext* opCtx,
                                          ScopedReceiveChunk scopedReceiveChunk,
                                          const MigrationRecipientRecoveryDocument& recoveryDoc);

    /**
     * Clones documents from a donor shard.
     */
    static repl::OpTime fetchAndApplyBatch(
        OperationContext* opCtx,
        std::function<bool(OperationContext*, BSONObj)> applyBatchFn,
        std::function<bool(OperationContext*, BSONObj*)> fetchBatchFn);

    /**
     * Idempotent method, which causes the current ongoing migration to abort only if it has the
     * specified session id. If the migration is already aborted, does nothing.
     */
    Status abort(const MigrationSessionId& sessionId);

    /**
     * Same as 'abort' above, but unconditionally aborts the current migration without checking the
     * session id. Only used for backwards compatibility.
     */
    void abortWithoutSessionIdCheck();

    /*
     * 'clearShardCatalogCache' records whether the recipient must refresh its filtering metadata
     * when it later releases the migration critical section. The authoritative path passes false
     * because the post-migration metadata is installed into the shard catalog directly.
     * TODO (SERVER-127253): Remove clearShardCatalogCache once v9.0 branches out.
     */
    Status startCommit(const MigrationSessionId& sessionId, bool clearShardCatalogCache);

    /*
     * Refreshes the filtering metadata and releases the migration recipient critical section for
     * the specified migration session. If no session is ongoing or the session doesn't match the
     * current one, it does nothing and returns OK.
     *
     * If clearShardCatalogCache is true, the filtering metadata is refreshed before releasing the
     * critical section. On the authoritative path it is false because the metadata has already been
     * installed into the shard catalog while the critical section was held.
     * TODO (SERVER-127253) Remove clearShardCatalogCache when v9.0 branches out.
     */
    Status exitCriticalSection(OperationContext* opCtx,
                               const MigrationSessionId& sessionId,
                               bool clearShardCatalogCache);

    /**
     * Gets the collection indexes from fromShardId. If given a chunk manager, will fetch the
     * indexes using the shard version protocol. if expandSimpleCollation is true, this will add
     * simple collation to a secondary index spec if the index spec has no collation.
     */
    struct IndexesAndIdIndex {
        std::vector<BSONObj> indexSpecs;
        BSONObj idIndexSpec;
    };
    [[MONGO_MOD_NEEDS_REPLACEMENT]] static IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ShardId& fromShardId,
        const boost::optional<CollectionRoutingInfo>& cri,
        boost::optional<Timestamp> afterClusterTime,
        bool expandSimpleCollation = false);

    /**
     * Gets the collection uuid and options from fromShardId. If given a chunk manager, will fetch
     * the collection options using the database version protocol.
     */
    struct CollectionOptionsAndUUID {
        BSONObj options;
        UUID uuid;
    };

    static CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        const ShardId& fromShardId,
        const boost::optional<DatabaseVersion>& dbVersion,
        boost::optional<Timestamp> afterClusterTime);

    /**
     * Creates the collection on the shard and clones the indexes and options.
     * If the collection already exists, it will be updated to match the target options and indexes,
     * including dropping any indexes not specified in the target index specs.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] static void cloneCollectionIndexesAndOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptionsAndIndexes& collectionOptionsAndIndexes);

    /**
     * Checks if any documents already exist in the given shard key range on the recipient shard.
     * This is used to detect orphaned documents that are present due to possible range deleter bugs
     * or unsupported manual operations on a direct connection.
     *
     * Returns the shard key of the first document found in the range, or boost::none if no
     * documents exist.
     */
    static boost::optional<BSONObj> checkForExistingDocumentsInRange(OperationContext* opCtx,
                                                                     const NamespaceString& nss,
                                                                     const UUID& collUuid,
                                                                     const BSONObj& shardKeyPattern,
                                                                     const BSONObj& min,
                                                                     const BSONObj& max);

    /**
     * Returns true if accepting a migration would drop point-in-time (PIT) reachable ownership
     * history from this shard's local catalog. In that case, the migration must be rejected.
     *
     * A migration performed through the MoveRangeCoordinator updates only the chunks that changed
     * in the shard catalog: the control chunk and the chunks split and migrated as part of the
     * move. The recipient is given the range that will be replaced on the shard catalog commit,
     * called 'enclosingChunk' (the original donor chunk that encloses the migrated range).
     *
     * History would be lost if the catalog holds a stored chunk that:
     *   - will be replaced when the recipient commits the move,
     *   - is not fully covered by 'enclosingChunk', and
     *   - is still reachable by PIT reads.
     * The uncovered portion of such a chunk has no replacement after the commit, so its ownership
     * history disappears.
     *
     * Example: this shard previously donated [0, 100) to another shard and still has a stale
     * [0, 100) entry. That chunk was later split into [0, 50) and [50, 100), but this shard is
     * unaware of the split because it no longer owns the chunk. If it later receives [50, 100)
     * back, committing the migration will delete the stale [0, 100) entry, which would lose
     * point-in-time read accessibility for the uncovered [0, 50) range.
     */
    static bool migrationWouldDropPITHistory(OperationContext* opCtx,
                                             const UUID& collUuid,
                                             const ShardId& recipientShardId,
                                             const ChunkRange& enclosingChunk);

    /**
     * Enforces that accepting a migration over 'enclosingChunk' does not drop point-in-time
     * (PIT) reachable ownership history from this shard's local catalog (see
     * migrationWouldDropPITHistory() above). By default aborts with
     * ConflictingOperationInProgress. If 'allowMigrationsToDropRecipientPITHistory' is enabled,
     * logs a warning and lets the migration proceed instead.
     */
    static void ensurePITHistoryPreserved(OperationContext* opCtx,
                                          const UUID& collUuid,
                                          const ShardId& recipientShardId,
                                          const ChunkRange& enclosingChunk,
                                          const NamespaceString& nss,
                                          const UUID& migrationId);

private:
    /**
     * Set state to Fail without Logging.
     * Under lock, move msg to _errmsg and set the state to FAIL.
     */
    void _setStateFailNoLog(std::string_view msg);

    /**
     * These log the argument msg; then call _setStateFailNoLog, which
     * under lock, moves msg to _errmsg and sets the state to FAIL.
     * The setStateWailWarn version logs with "warning() << msg".
     */
    void _setStateFail(std::string_view msg);
    void _setStateFailWarn(std::string_view msg);

    void _setState(State newState);

    /**
     * Thread which drives the migration apply process on the recipient side.
     */
    void _migrateThread(CancellationToken cancellationToken, bool skipToCritSecTaken = false);

    void _migrateDriver(OperationContext* opCtx, bool skipToCritSecTaken = false);

    bool _applyMigrateOp(OperationContext* opCtx, const BSONObj& xfer);

    bool _flushPendingWrites(OperationContext* opCtx, const repl::OpTime& lastOpApplied);

    /**
     * Remembers a chunk range between 'min' and 'max' as a range which will have data migrated
     * into it, to protect it against separate commands to clean up orphaned data. First, though,
     * it schedules deletion of any documents in the range, so that process must be seen to be
     * complete before migrating any new documents in.
     */
    SharedSemiFuture<void> _notePending(OperationContext*, ChunkRange const&);

    /**
     * Stops tracking a chunk range between 'min' and 'max' that previously was having data
     * migrated into it, and schedules deletion of any such documents already migrated in.
     */
    void _forgetPending(OperationContext*, ChunkRange const&);

    /**
     * Checks whether the MigrationDestinationManager is currently handling a migration by checking
     * that the migration "_sessionId" is initialized.
     */
    bool _isActive(WithLock) const;

    /**
     * Waits for _state to transition to EXIT_CRIT_SEC. Then, it performs a filtering metadata
     * refresh, releases the critical section and finally deletes the recovery document.
     */
    void awaitCriticalSectionReleaseSignalAndCompleteMigration(OperationContext* opCtx,
                                                               const Timer& timeInCriticalSection);

    /**
     * Called by onShutdown/onStepDown hooks to cancel and join migrateThread.
     */
    void _cancelAndJoinMigrateThread();

    /**
     * ReplicaSetAwareService entry points.
     */
    void onStartup(OperationContext* opCtx) final {}
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}
    void onShutdown() final;
    void onStepUpBegin(OperationContext* opCtx, long long term) final;
    void onStepUpComplete(OperationContext* opCtx, long long term) final {}
    void onStepDown() final;
    void onRollbackBegin() final {}
    void onBecomeArbiter() final {}
    inline std::string getServiceName() const final {
        return "MigrationDestinationManager";
    }

    // The number of session oplog entries recieved from the source shard. Not all oplog
    // entries recieved from the source shard may be committed
    AtomicWord<long long> _sessionOplogEntriesMigrated{0};

    // Mutex to guard all fields below
    mutable std::mutex _mutex;

    // Migration session ID uniquely identifies the migration and indicates whether the prepare
    // method has been called.
    boost::optional<MigrationSessionId> _sessionId;
    boost::optional<ScopedReceiveChunk> _scopedReceiveChunk;

    // A condition variable on which to wait for the prepare method to be called.
    stdx::condition_variable _isActiveCV;

    stdx::thread _migrateThreadHandle;

    long long _getNumCloned() {
        return _migrationCloningProgress ? _migrationCloningProgress->getNumCloned() : 0;
    }

    long long _getNumBytesCloned() {
        return _migrationCloningProgress ? _migrationCloningProgress->getNumBytes() : 0;
    }

    boost::optional<UUID> _migrationId;
    boost::optional<UUID> _collectionUuid;

    // State that is shared among all inserter threads.
    std::shared_ptr<MigrationCloningProgressSharedState> _migrationCloningProgress;

    LogicalSessionId _lsid;
    TxnNumber _txnNumber{kUninitializedTxnNumber};
    NamespaceString _nss;
    ConnectionString _fromShardConnString;
    ShardId _fromShard;
    ShardId _toShard;

    BSONObj _min;
    BSONObj _max;

    // The donor chunk that encloses the migrated range [_min, _max).
    // Set in start() from the _recvChunkStart command. Present only on the authoritative path
    // (driven by the MoveRangeCoordinator); its presence is the signal to run the shard-catalog
    // PIT-reachability check. Absent on requests from a pre-upgrade donor and on the legacy path,
    // which skip that check.
    //
    // TODO (SERVER-127253) Make this parameter non-optional once v9.0 branches out.
    boost::optional<ChunkRange> _enclosingChunk;

    // Whether the migration commits authoritatively (driven by a MoveRangeCoordinator), as reported
    // by the donor in the _recvChunkStart request.
    bool _isAuthoritative{false};

    BSONObj _shardKeyPattern;

    WriteConcernOptions _writeConcern;

    // Set to true once we have accepted the chunk as pending into our metadata. Used so that on
    // failure we can perform the appropriate cleanup.
    bool _chunkMarkedPending{false};

    long long _numCatchup{0};
    long long _numSteady{0};

    State _state{kReady};
    std::string _errmsg;

    // Whether the recipient must refresh its filtering metadata when it releases the migration
    // critical section. Set by the donor when committing (see startCommit) and persisted in the
    // recipient recovery document so the value survives failover. Defaults to true to preserve the
    // legacy refresh behavior.
    // TODO (SERVER-127253): Remove once v9.0 branches out
    bool _clearShardCatalogCache{true};

    std::unique_ptr<SessionCatalogMigrationDestination> _sessionMigration;

    // Condition variable, which is signalled every time the state of the migration changes.
    stdx::condition_variable _stateChangedCV;

    // Promise that will be fulfilled when the donor has signaled us that we can release the
    // critical section.
    // Resolves with whether the recipient should refresh its filtering metadata before releasing
    // the critical section (true) or skip the refresh on the authoritative path (false).
    std::unique_ptr<SharedPromise<bool>> _canReleaseCriticalSectionPromise;

    // Promise that will be fulfilled when the migrateThread has finished its work.
    std::unique_ptr<SharedPromise<State>> _migrateThreadFinishedPromise;

    // Cancellation source that is cancelled on stepdown, shutdown, or explicit
    // abort — all cases where ongoing migration work should stop immediately. On stepup, a new
    // cancellation source will be installed.
    CancellationSource _cancellationSource;
};

}  // namespace mongo
