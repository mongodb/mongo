/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_oplog_applier.h"
#include "mongo/db/serverless/serverless_types_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {

class DBClientConnection;
class OperationContext;
class ReplicaSetMonitor;
class ServiceContext;

namespace repl {
class OplogBufferCollection;

/**
 * ShardMergeRecipientService is a primary only service which orchestrates the
 * data migration on the recipient side for shard merge protocol.
 */
class ShardMergeRecipientService final : public PrimaryOnlyService {
    // Disallows copying.
    ShardMergeRecipientService(const ShardMergeRecipientService&) = delete;
    ShardMergeRecipientService& operator=(const ShardMergeRecipientService&) = delete;

public:
    static constexpr StringData kShardMergeRecipientServiceName = "ShardMergeRecipientService"_sd;

    explicit ShardMergeRecipientService(ServiceContext* serviceContext);
    ~ShardMergeRecipientService() override = default;

    StringData getServiceName() const final;

    NamespaceString getStateDocumentsNS() const final;

    ThreadPool::Limits getThreadPoolLimits() const final;

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialStateDoc,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) final;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialStateDoc) final;

    /**
     * Interrupts all shard merge recipient service instances.
     */
    void abortAllMigrations(OperationContext* opCtx);

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        explicit Instance(ServiceContext* serviceContext,
                          const ShardMergeRecipientService* recipientService,
                          BSONObj stateDoc);

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancellationToken& token) noexcept final;

        /**
         * Unconditional migration interrupt called on node's stepdown/shutdown event.
         * Make the instance to not wait for `recipientForgetMigration` command.
         */
        void interrupt(Status status) override;

        /**
         * Conditional migration interrupt called on fcv change or due to oplog fetcher error.
         * Make the instance to wait for `recipientForgetMigration` command.
         */
        void interruptConditionally(Status status);

        /**
         * Interrupts the migration for garbage collection.
         */
        void onReceiveRecipientForgetMigration(OperationContext* opCtx,
                                               const MigrationDecisionEnum& decision);

        /**
         * Returns a Future that will be resolved when migration is completed.
         */
        SharedSemiFuture<void> getMigrationCompletionFuture() const {
            return _migrationCompletionPromise.getFuture();
        }

        /**
         * Returns a Future that will be resolved when the instance has been durably marked garbage
         * collectable.
         */
        SharedSemiFuture<void> getForgetMigrationDurableFuture() const {
            return _forgetMigrationDurablePromise.getFuture();
        }

        /**
         *  Returns the instance id.
         */
        const UUID& getMigrationUUID() const;

        /**
         * Returns the instance state document.
         */
        ShardMergeRecipientDocument getStateDoc() const;

        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final;

        void checkIfOptionsConflict(const BSONObj& stateDoc) const final;

        /**
         * Blocks the thread until the migration reaches consistent state in an interruptible
         * mode.
         *
         * Returns the donor OpTime at which the migration reached consistent state. Throws
         * exception on error.
         */
        OpTime waitUntilMigrationReachesConsistentState(OperationContext* opCtx) const;

        /**
         * Blocks the thread until the tenant oplog applier applied data past the
         * 'returnAfterReachingTimestamp' in an interruptible mode. If the recipient's logical clock
         * has not yet reached the 'returnAfterReachingTimestamp', advances the recipient's logical
         * clock to 'returnAfterReachingTimestamp'. Finally, stores the
         * 'returnAfterReachingTimestamp' as 'rejectReadsBeforeTimestamp' in the state
         * document and waits for the write to be replicated to every node (i.e. wait for
         * 'rejectReadsBeforeTimestamp' to be set on the TenantMigrationRecipientAccessBlocker of
         * every node) to guarantee that no reads will be incorrectly accepted.
         */
        OpTime waitUntilMigrationReachesReturnAfterReachingTimestamp(
            OperationContext* opCtx, const Timestamp& returnAfterReachingTimestamp);

        /**
         * Called when a replica set member (self, or a secondary) finishes importing donated files.
         */
        void onMemberImportedFiles(const HostAndPort& host);

        /**
         * Set the oplog creator functor, to allow use of a mock oplog fetcher.
         */
        void setCreateOplogFetcherFn_forTest(
            std::unique_ptr<OplogFetcherFactory>&& createOplogFetcherFn) {
            _createOplogFetcherFn = std::move(createOplogFetcherFn);
        }

        /**
         * Stops the oplog applier without going through recipientForgetMigration.
         */
        void stopOplogApplier_forTest() {
            stdx::lock_guard lk(_mutex);
            _tenantOplogApplier->shutdown();
        }

        /**
         * Suppresses selecting 'host' as the donor sync source, until 'until'.
         */
        void excludeDonorHost_forTest(const HostAndPort& host, Date_t until) {
            stdx::lock_guard lk(_mutex);
            _excludeDonorHost(lk, host, until);
        }

        const auto& getExcludedDonorHosts_forTest() {
            return _excludedDonorHosts;
        }

    private:
        friend class ShardMergeRecipientServiceTest;

        /**
         * Only used for testing. Allows setting a custom task executor for backup cursor fetcher.
         */
        void setBackupCursorFetcherExecutor_forTest(
            std::shared_ptr<executor::TaskExecutor> taskExecutor) {
            _backupCursorExecutor = std::move(taskExecutor);
        }

        const NamespaceString _stateDocumentsNS = NamespaceString::kShardMergeRecipientsNamespace;

        using ConnectionPair =
            std::pair<std::unique_ptr<DBClientConnection>, std::unique_ptr<DBClientConnection>>;

        /**
         * Transitions the instance state to 'kStarted' if the state is uninitialized.
         */
        SemiFuture<void> _initializeAndDurablyPersistStateDoc();

        /**
         * Execute steps which are necessary to start a migration, such as, establishing donor
         * client connection, setting up internal state, get donor cluster keys, etc.
         */
        SemiFuture<void> _prepareForMigration(const CancellationToken& token);

        /**
         * Sets up internal state to begin migration.
         */
        void _setup(ConnectionPair connectionPair);

        /**
         * Start migration only if the following FCV checks passes:
         * a) Not in middle of FCV upgrading/downgrading.
         * b) Donor and recipient FCV matches.
         */
        SemiFuture<void> _startMigrationIfSafeToRunwithCurrentFCV(const CancellationToken& token);

        /**
         * Helper to run FCV sanity checks at the start of migration.
         */
        void _assertIfMigrationIsSafeToRunWithCurrentFcv();

        /**
         * Waits for all data bearing nodes to complete import.
         */
        SemiFuture<void> _waitForAllNodesToFinishImport();

        /**
         * Tells whether the migration is committed or aborted.
         */
        bool _isCommitOrAbortState(WithLock) const;

        /**
         * Waits for recipientForgetMigartion command for migration decision and then, mark external
         * keys doc and instance state doc as garbage collectable.
         */
        SemiFuture<void> _waitForForgetMigrationThenMarkMigrationGarbageCollectable(
            const CancellationToken& token);

        /**
         * Durably persists the migration decision in the state doc.
         */
        SemiFuture<void> _durablyPersistCommitAbortDecision(MigrationDecisionEnum decision);

        /*
         * Drops ephemeral collections used for migrations after migration decision is durably
         * persisted.
         */
        void _dropTempCollections();

        /**
         * Sets the `expireAt` field at the state doc.
         */
        SemiFuture<void> _markStateDocAsGarbageCollectable();

        /**
         * Deletes the state document. Does not return the opTime for the delete, since it's not
         * necessary to wait for this delete to be majority committed (this is one of the last steps
         * in the chain, and if the delete rolls back, the new primary will re-do the delete).
         */
        SemiFuture<void> _removeStateDoc(const CancellationToken& token);

        SemiFuture<void> _waitForGarbageCollectionDelayThenDeleteStateDoc(
            const CancellationToken& token);

        /**
         * Creates a client, connects it to the donor and uses the default
         * authentication mode (KeyFile Authentication). Throws a user assertion on failure.
         */
        std::unique_ptr<DBClientConnection> _connectAndAuth(const HostAndPort& serverAddress,
                                                            StringData applicationName);

        /**
         * Creates and connects both the oplog fetcher client and the client used for other
         * operations.
         */
        SemiFuture<ConnectionPair> _createAndConnectClients();

        /**
         * Fetches all key documents from the donor's admin.system.keys collection, stores them in
         * config.external_validation_keys, and refreshes the keys cache.
         */
        void _fetchAndStoreDonorClusterTimeKeyDocs(const CancellationToken& token);

        /**
         * Opens a backup cursor on the donor primary and fetches the
         * list of donor files to be cloned.
         */
        SemiFuture<void> _openBackupCursor(const CancellationToken& token);
        SemiFuture<void> _openBackupCursorWithRetry(const CancellationToken& token);

        /**
         * Keeps the donor backup cursor alive.
         */
        void _keepBackupCursorAlive(const CancellationToken& token);

        /**
         * Kills the backup cursor opened on donor, if any.
         *
         */
        void _killBackupCursor();

        /**
         * Gets the backup cursor metadata info.
         */
        const BackupCursorInfo& _getDonorBackupCursorInfo(WithLock) const;

        /**
         * Get the oldest active multi-statement transaction optime by reading
         * config.transactions collection at given ReadTimestamp (i.e, equal to
         * startApplyingDonorOpTime) snapshot.
         */
        boost::optional<OpTime> _getOldestActiveTransactionAt(Timestamp ReadTimestamp);

        /**
         * Retrieves the start/fetch optimes from the donor and updates the in-memory/on-disk states
         * accordingly.
         */
        SemiFuture<void> _getStartOpTimesFromDonor();

        /**
         * Pushes documents from oplog fetcher to oplog buffer.
         *
         * Returns a status even though it always returns OK, to conform the interface OplogFetcher
         * expects for the EnqueueDocumentsFn.
         */
        Status _enqueueDocuments(OplogFetcher::Documents::const_iterator begin,
                                 OplogFetcher::Documents::const_iterator end,
                                 const OplogFetcher::DocumentsInfo& info);

        /**
         * Creates the oplog buffer that will be populated by donor oplog entries from the retryable
         * writes fetching stage and oplog fetching stage.
         */
        void _createOplogBuffer(WithLock, OperationContext* opCtx);

        /**
         * Runs an aggregation that gets the entire oplog chain for every retryable write entry in
         * `config.transactions`. Only returns oplog entries in the chain where
         * `ts` < `startFetchingOpTime.ts` and adds them to the oplog buffer.
         */
        SemiFuture<void> _fetchRetryableWritesOplogBeforeStartOpTime();

        /**
         * Migrates committed transactions entries into 'config.transactions'.
         */
        SemiFuture<void> _fetchCommittedTransactionsBeforeStartOpTime();

        /**
         * Opens and returns a cursor for all entries with 'lastWriteOpTime' <=
         * 'startApplyingDonorOpTime' and state 'committed'.
         */
        std::unique_ptr<DBClientCursor> _openCommittedTransactionsFindCursor();

        /**
         * Creates an aggregation pipeline to fetch transaction entries with 'lastWriteOpTime' <
         * 'startFetchingDonorOpTime' and 'state: committed'.
         */
        AggregateCommandRequest _makeCommittedTransactionsAggregation() const;

        /**
         * Processes a committed transaction entry from the donor. Updates the recipient's
         * 'config.transactions' collection with the entry and writes a no-op entry for the
         * recipient secondaries to replicate the entry.
         */
        void _processCommittedTransactionEntry(const BSONObj& entry);

        /**
         * Starts the oplog buffer only if the node is primary. Otherwise, throw error.
         */
        void _startOplogBuffer(OperationContext* opCtx);

        /**
         * Starts the tenant oplog fetcher.
         */
        void _startOplogFetcher();

        /**
         * Called when the oplog fetcher finishes. Usually the oplog fetcher finishes only when
         * cancelled or on error.
         */
        void _oplogFetcherCallback(Status oplogFetcherStatus);

        /**
         * Starts the tenant oplog applier.
         */
        void _startOplogApplier();

        /**
         * Waits for tenant oplog applier to stop.
         */
        SemiFuture<TenantOplogApplier::OpTimePair> _waitForMigrationToComplete();

        /**
         * Advances the majority commit timestamp to be >= donor's backup cursor checkpoint
         * timestamp(CkptTs) by:
         * 1. Advancing the clusterTime to CkptTs.
         * 2. Writing a no-op oplog entry with ts > CkptTs
         * 3. Waiting for the majority commit timestamp to be the time of the no-op write.
         *
         * Notes: This method should be called before transitioning the instance state to
         * 'kLearnedFilenames' which causes donor collections to get imported. Current import rule
         * is that the import table's checkpoint timestamp can't be later than the recipient's
         * stable timestamp. Due to the fact, we don't have a mechanism to wait until a specific
         * stable timestamp on a given node or set of nodes in the replica set and the majority
         * commit point and stable timestamp aren't atomically updated, advancing the majority
         * commit point on the recipient before import collection stage is a best-effort attempt to
         * prevent import retry attempts on import timestamp rule violation.
         */
        SemiFuture<void> _advanceMajorityCommitTsToBkpCursorCheckpointTs(
            const CancellationToken& token);

        /**
         * Returns a future that will be fulfilled when the tenant migration reaches consistent
         * state.
         */
        SemiFuture<void> _getDataConsistentFuture();

        /**
         * Transitions the instance state to 'kLearnedFilenames' after learning all filenames to be
         * imported.
         */
        SemiFuture<void> _enterLearnedFilenamesState();

        /**
         * Durably persist that migration has reached consistent state and signal waiters.
         */
        SemiFuture<void> _enterConsistentState();
        SemiFuture<void> _durablyPersistConsistentState();

        /**
         * Gets the migration interrupt status. Answers may change after this call as it reads the
         * interrupt status without holding mutex lock. It's the caller's responsibility to decide
         * if they need to hold mutex lock or not before calling the method.
         */
        Status _getInterruptStatus() const;

        /**
         * Cancels all remaining work in the migration.
         */
        void _cancelRemainingWork(WithLock lk, Status status);

        /**
         * Performs some cleanup work on migration completion, like, shutting down the components or
         * fulfilling any instance promises.
         */
        void _cleanupOnMigrationCompletion(Status status);

        /**
         * Suppresses selecting 'host' as the donor sync source, until 'until'.
         */
        void _excludeDonorHost(WithLock, const HostAndPort& host, Date_t until);

        /**
         * Returns a vector of currently excluded donor hosts. Also removes hosts from the list of
         * excluded donor nodes, if the exclude duration has expired.
         */
        std::vector<HostAndPort> _getExcludedDonorHosts(WithLock);

        /**
         * Makes the failpoint stop or hang the migration based on failpoint data "action" field.
         * If "action" is "hang" and 'opCtx' is not null, the failpoint will be interruptible.
         */
        void _stopOrHangOnFailPoint(FailPoint* fp, OperationContext* opCtx = nullptr);

        /**
         * Updates the shard merge recipient state doc and waits for that change to be
         * propagated to a majority.
         */
        SemiFuture<void> _updateStateDocForMajority(WithLock lk);

        /**
         * Updates the shard merge recipient state doc. Throws error if it fails to
         * update.
         */
        void _updateStateDoc(OperationContext* opCtx, const ShardMergeRecipientDocument& stateDoc);

        /**
         * Returns the majority OpTime on the donor node that 'client' is connected to.
         */
        OpTime _getDonorMajorityOpTime(std::unique_ptr<mongo::DBClientConnection>& client);

        mutable stdx::mutex _mutex;

        // All member variables are labeled with one of the following codes indicating the
        // synchronization rules for accessing them.
        //
        // (R)  Read-only in concurrent operation; no synchronization required.
        // (S)  Self-synchronizing; access according to class's own rules.
        // (M)  Reads and writes guarded by _mutex.
        // (W)  Synchronization required only for writes.

        ServiceContext* const _serviceContext;
        const ShardMergeRecipientService* const _recipientService;      // (R) (not owned)
        std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;  // (M)
        std::shared_ptr<executor::TaskExecutor> _backupCursorExecutor;  // (M)
        ShardMergeRecipientDocument _stateDoc;                          // (M)

        // This data is provided in the initial state doc and never changes.  We keep copies to
        // avoid having to obtain the mutex to access them.
        const std::vector<TenantId> _tenantIds;       // (R)
        const UUID _migrationUuid;                    // (R)
        const std::string _donorConnectionString;     // (R)
        const MongoURI _donorUri;                     // (R)
        const ReadPreferenceSetting _readPreference;  // (R)

        std::shared_ptr<ReplicaSetMonitor> _donorReplicaSetMonitor;  // (M)

        // Members of the donor replica set that we have excluded as a potential sync source for
        // some period of time.
        std::vector<std::pair<HostAndPort, Date_t>> _excludedDonorHosts;  // (M)

        // The '_client' will be used for other operations such as fetching
        // optimes while the '_oplogFetcherClient' will be reserved for the oplog fetcher only.
        // Because the oplog fetcher uses exhaust, we need a dedicated connection for oplog fetcher.
        //
        // Follow DBClientCursor synchonization rules.
        std::unique_ptr<DBClientConnection> _client;              // (S)
        std::unique_ptr<DBClientConnection> _oplogFetcherClient;  // (S)

        std::unique_ptr<Fetcher> _donorFilenameBackupCursorFileFetcher;  // (M)
        CancellationSource _backupCursorKeepAliveCancellation = {};      // (X)
        boost::optional<SemiFuture<void>> _backupCursorKeepAliveFuture;  // (M)

        std::unique_ptr<OplogFetcherFactory> _createOplogFetcherFn =
            std::make_unique<CreateOplogFetcherFn>();                               // (M)
        std::unique_ptr<OplogBufferCollection> _donorOplogBuffer;                   // (M)
        std::unique_ptr<DataReplicatorExternalState> _dataReplicatorExternalState;  // (M)
        std::unique_ptr<OplogFetcher> _donorOplogFetcher;                           // (M)
        std::shared_ptr<TenantOplogApplier> _tenantOplogApplier;                    // (M)

        // Writer pool to do storage write operation. Used by tenant collection cloner and by
        // tenant oplog applier.
        std::unique_ptr<ThreadPool> _workerPool;  //(M)
        // Data shared by cloners. Follow TenantMigrationSharedData synchronization rules.
        std::unique_ptr<TenantMigrationSharedData> _sharedData;  // (S)

        // Promise that is resolved when all voting data-bearing recipient nodes have successfully
        // imported all donor files.
        SharedPromise<void> _importQuorumPromise;  // (W)
        // Whether we are waiting for members to import donor files.
        bool _waitingForMembersToImportFiles = true;
        // Which members have imported all donor files.
        stdx::unordered_set<HostAndPort> _membersWhoHaveImportedFiles;

        // Promise that is resolved when the migration reached consistent point.
        SharedPromise<OpTime> _dataConsistentPromise;  // (W)
        // Promise that is resolved when migration is completed.
        SharedPromise<void> _migrationCompletionPromise;  // (W)
        // Promise that is resolved when the recipientForgetMigration command is received or on
        // stepDown/shutDown with errors.
        SharedPromise<MigrationDecisionEnum> _receivedRecipientForgetMigrationPromise;  // (W)
        // Promise that is resolved when the instance has been durably marked garbage collectable.
        SharedPromise<void> _forgetMigrationDurablePromise;  // (W)
        // Promise that is resolved with when the instance is interrupted, and holds interrupt error
        // status.
        SharedPromise<void> _interruptPromise;  // (M)

        // Waiters are notified when 'tenantOplogApplier' is valid on restart.
        stdx::condition_variable _restartOplogApplierCondVar;  // (M)
        // Waiters are notified when 'tenantOplogApplier' is ready to use.
        stdx::condition_variable _oplogApplierReadyCondVar;  // (M)
        // Indicates whether 'tenantOplogApplier' is ready to use or not.
        bool _oplogApplierReady = false;  // (M)
    };

private:
    /**
     *  Creates the state document collection.
     */
    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override;

    ServiceContext* const _serviceContext;
};
}  // namespace repl
}  // namespace mongo
