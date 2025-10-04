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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_batch_fetcher.h"
#include "mongo/db/s/migration_batch_inserter.h"
#include "mongo/db/s/migration_recipient_recovery_document_gen.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration_destination.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <memory>
#include <string>
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
class MigrationDestinationManager
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

    Status startCommit(const MigrationSessionId& sessionId);

    /*
     * Refreshes the filtering metadata and releases the migration recipient critical section for
     * the specified migration session. If no session is ongoing or the session doesn't match the
     * current one, it does nothing and returns OK.
     */
    Status exitCriticalSection(OperationContext* opCtx, const MigrationSessionId& sessionId);

    /**
     * Gets the collection indexes from fromShardId. If given a chunk manager, will fetch the
     * indexes using the shard version protocol. if expandSimpleCollation is true, this will add
     * simple collation to a secondary index spec if the index spec has no collation.
     */
    struct IndexesAndIdIndex {
        std::vector<BSONObj> indexSpecs;
        BSONObj idIndexSpec;
    };
    static IndexesAndIdIndex getCollectionIndexes(OperationContext* opCtx,
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
    static void cloneCollectionIndexesAndOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptionsAndIndexes& collectionOptionsAndIndexes);

private:
    /**
     * These log the argument msg; then, under lock, move msg to _errmsg and set the state to FAIL.
     * The setStateWailWarn version logs with "warning() << msg".
     */
    void _setStateFail(StringData msg);
    void _setStateFailWarn(StringData msg);

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
     * ReplicaSetAwareService entry points.
     */
    void onStartup(OperationContext* opCtx) final {}
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}
    void onShutdown() final {}
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
    mutable stdx::mutex _mutex;

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
    BSONObj _shardKeyPattern;

    WriteConcernOptions _writeConcern;

    // Set to true once we have accepted the chunk as pending into our metadata. Used so that on
    // failure we can perform the appropriate cleanup.
    bool _chunkMarkedPending{false};

    long long _numCatchup{0};
    long long _numSteady{0};

    State _state{kReady};
    std::string _errmsg;

    std::unique_ptr<SessionCatalogMigrationDestination> _sessionMigration;

    // Condition variable, which is signalled every time the state of the migration changes.
    stdx::condition_variable _stateChangedCV;

    // Promise that will be fulfilled when the donor has signaled us that we can release the
    // critical section.
    std::unique_ptr<SharedPromise<void>> _canReleaseCriticalSectionPromise;

    // Promise that will be fulfilled when the migrateThread has finished its work.
    std::unique_ptr<SharedPromise<State>> _migrateThreadFinishedPromise;

    // Cancellation source that is cancelled on stepdowns. On stepup, a new cancellation source will
    // be installed.
    CancellationSource _cancellationSource;
};

}  // namespace mongo
