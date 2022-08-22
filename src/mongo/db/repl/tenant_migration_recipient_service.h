/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include <memory>

#include "mongo/client/fetcher.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/tenant_all_database_cloner.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_oplog_applier.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/time_support.h"

namespace mongo {

class DBClientConnection;
class OperationContext;
class ReplicaSetMonitor;
class ServiceContext;

namespace repl {
class OplogBufferCollection;
/**
 * TenantMigrationRecipientService is a primary only service to handle
 * data copy portion of a multitenant migration on recipient side.
 */
class TenantMigrationRecipientService final : public PrimaryOnlyService {
    // Disallows copying.
    TenantMigrationRecipientService(const TenantMigrationRecipientService&) = delete;
    TenantMigrationRecipientService& operator=(const TenantMigrationRecipientService&) = delete;

public:
    static constexpr StringData kTenantMigrationRecipientServiceName =
        "TenantMigrationRecipientService"_sd;
    static constexpr StringData kNoopMsg = "Resume token noop"_sd;

    explicit TenantMigrationRecipientService(ServiceContext* serviceContext);
    ~TenantMigrationRecipientService() = default;

    StringData getServiceName() const final;

    NamespaceString getStateDocumentsNS() const final;

    ThreadPool::Limits getThreadPoolLimits() const final;

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialStateDoc,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) final;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialStateDoc) final;

    /**
     * Sends an abort to all tenant migration instances on this recipient.
     */
    void abortAllMigrations(OperationContext* opCtx);

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        explicit Instance(ServiceContext* serviceContext,
                          const TenantMigrationRecipientService* recipientService,
                          BSONObj stateDoc);

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancellationToken& token) noexcept final;

        /*
         * Interrupts the running instance and cause the completion future to complete with
         * 'status'.
         */
        void interrupt(Status status) override;

        /*
         * Cancels the running instance but permits waiting for forgetMigration.
         */
        void cancelMigration();

        /**
         * Interrupts the migration for garbage collection.
         */
        void onReceiveRecipientForgetMigration(OperationContext* opCtx);

        /**
         * Returns a Future that will be resolved when data sync associated with this Instance has
         * completed running.
         */
        SharedSemiFuture<void> getDataSyncCompletionFuture() const {
            return _dataSyncCompletionPromise.getFuture();
        }

        /**
         * Returns a Future that will be resolved when the instance has been durably marked garbage
         * collectable.
         */
        SharedSemiFuture<void> getForgetMigrationDurableFuture() const {
            return _forgetMigrationDurablePromise.getFuture();
        }

        /**
         * Report TenantMigrationRecipientService Instances in currentOp().
         */
        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final;

        /*
         *  Returns the instance id.
         */
        const UUID& getMigrationUUID() const;

        /*
         *  Returns the tenant id (database prefix).
         */
        const std::string& getTenantId() const;

        /*
         *  Returns the migration protocol.
         */
        const MigrationProtocolEnum& getProtocol() const;

        /*
         * Returns the recipient document state.
         */
        TenantMigrationRecipientDocument getState() const;

        void checkIfOptionsConflict(const BSONObj& stateDoc) const final;

        /*
         * Blocks the thread until the tenant migration reaches consistent state in an interruptible
         * mode. Returns the donor optime at which the migration reached consistent state. Throws
         * exception on error.
         */
        OpTime waitUntilMigrationReachesConsistentState(OperationContext* opCtx) const;

        /*
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

        /*
         * Called when a replica set member (self, or a secondary) finishes importing donated files.
         */
        void onMemberImportedFiles(const HostAndPort& host,
                                   bool success,
                                   const boost::optional<StringData>& reason = boost::none);

        /*
         *  Set the oplog creator functor, to allow use of a mock oplog fetcher.
         */
        void setCreateOplogFetcherFn_forTest(
            std::unique_ptr<OplogFetcherFactory>&& createOplogFetcherFn) {
            _createOplogFetcherFn = std::move(createOplogFetcherFn);
        }

        /**
         * Stops the oplog applier without going through tenantForgetMigration.
         */
        void stopOplogApplier_forTest() {
            stdx::lock_guard lk(_mutex);
            _tenantOplogApplier->shutdown();
        }

        /*
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
        friend class TenantMigrationRecipientServiceTest;

        using ConnectionPair =
            std::pair<std::unique_ptr<DBClientConnection>, std::unique_ptr<DBClientConnection>>;

        // Represents the instance task state.
        class TaskState {
        public:
            enum StateFlag {
                kNotStarted = 1 << 0,
                kRunning = 1 << 1,
                kInterrupted = 1 << 2,
                kDone = 1 << 3,
            };

            using StateSet = int;
            bool isSet(StateSet stateSet) const {
                return _state & stateSet;
            }

            bool checkIfValidTransition(StateFlag newState) {
                switch (_state) {
                    case kNotStarted:
                        return newState == kRunning || newState == kInterrupted ||
                            newState == kDone;
                    case kRunning:
                        return newState == kInterrupted || newState == kDone;
                    case kInterrupted:
                        return newState == kDone || newState == kRunning;
                    case kDone:
                        return false;
                }
                MONGO_UNREACHABLE;
            }

            void setState(StateFlag state, boost::optional<Status> interruptStatus = boost::none) {
                invariant(checkIfValidTransition(state),
                          str::stream() << "current state: " << toString(_state)
                                        << ", new state: " << toString(state));

                // The interruptStatus can exist (and should be non-OK) if and only if the state is
                // kInterrupted.
                invariant((state == kInterrupted && interruptStatus && !interruptStatus->isOK()) ||
                              (state != kInterrupted && !interruptStatus),
                          str::stream() << "new state: " << toString(state)
                                        << ", interruptStatus: " << interruptStatus);

                _state = state;
                _interruptStatus = (interruptStatus) ? interruptStatus.get() : _interruptStatus;
            }

            bool isNotStarted() const {
                return _state == kNotStarted;
            }

            bool isRunning() const {
                return _state == kRunning;
            }

            bool isInterrupted() const {
                return _state == kInterrupted;
            }

            bool isDone() const {
                return _state == kDone;
            }

            Status getInterruptStatus() const {
                return _interruptStatus;
            }

            std::string toString() const {
                return toString(_state);
            }

            static std::string toString(StateFlag state) {
                switch (state) {
                    case kNotStarted:
                        return "Not started";
                    case kRunning:
                        return "Running";
                    case kInterrupted:
                        return "Interrupted";
                    case kDone:
                        return "Done";
                }
                MONGO_UNREACHABLE;
            }

        private:
            // task state.
            StateFlag _state = kNotStarted;
            // task interrupt status. Set to Status::OK() only when the recipient service has not
            // been interrupted so far, and is used to remember the initial interrupt error.
            Status _interruptStatus = Status::OK();
        };

        /*
         * Helper for interrupt().
         * The _receivedForgetMigrationPromise is resolved when skipWaitingForForgetMigration is
         * set (e.g. stepDown/shutDown). And we use skipWaitingForForgetMigration=false for
         * interruptions coming from the instance's task chain itself (e.g. _oplogFetcherCallback).
         */
        void _interrupt(Status status, bool skipWaitingForForgetMigration);

        /*
         * Transitions the instance state to 'kStarted'.
         *
         * Persists the instance state doc and waits for it to be majority replicated.
         * Throws an user assertion on failure.
         */
        SemiFuture<void> _initializeStateDoc(WithLock);

        /*
         * Transitions the instance state to 'kDone' and sets the expireAt field.
         *
         * Persists the instance state doc and waits for it to be majority replicated.
         * Throws on shutdown / notPrimary errors.
         */
        SemiFuture<void> _markStateDocAsGarbageCollectable();

        /**
         * Creates a client, connects it to the donor. If '_transientSSLParams' is not none, uses
         * the migration certificate to do SSL authentication. Otherwise, uses the default
         * authentication mode. Throws a user assertion on failure.
         *
         */
        std::unique_ptr<DBClientConnection> _connectAndAuth(const HostAndPort& serverAddress,
                                                            StringData applicationName);

        /**
         * Creates and connects both the oplog fetcher client and the client used for other
         * operations.
         */
        SemiFuture<void> _createAndConnectClients();

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
         * Kills the Donor backup cursor
         */
        SemiFuture<void> _killBackupCursor();

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
         * Opens and returns a cursor for entries from '_makeCommittedTransactionsAggregation()'.
         */
        std::unique_ptr<DBClientCursor> _openCommittedTransactionsAggregationCursor();

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
         * Starts the tenant oplog fetcher.
         */
        void _startOplogFetcher();

        /**
         * Called when the oplog fetcher finishes.  Usually the oplog fetcher finishes only when
         * cancelled or on error.
         */
        void _oplogFetcherCallback(Status oplogFetcherStatus);

        /**
         * Returns the filter used to get only oplog documents related to the appropriate tenant.
         */
        BSONObj _getOplogFetcherFilter() const;

        /*
         * Traverse backwards through the oplog to find the optime which tenant oplog application
         * should resume from. The oplog applier should resume applying entries that have a greater
         * optime than the returned value.
         */
        OpTime _getOplogResumeApplyingDonorOptime(const OpTime& cloneFinishedRecipientOpTime) const;

        /*
         * Starts the tenant cloner.
         * Returns future that will be fulfilled when the cloner completes.
         */
        Future<void> _startTenantAllDatabaseCloner(WithLock lk);

        /*
         * Starts the tenant oplog applier.
         */
        void _startOplogApplier();

        /*
         * Waits for tenant oplog applier to stop.
         */
        SemiFuture<TenantOplogApplier::OpTimePair> _waitForOplogApplierToStop();

        /*
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

        /*
         * Gets called when the logical/file cloner completes cloning data successfully.
         * And, it is responsible to populate the 'dataConsistentStopDonorOpTime'
         * and 'cloneFinishedRecipientOpTime' fields in the state doc.
         */
        SemiFuture<void> _onCloneSuccess();

        /*
         * Returns a future that will be fulfilled when the tenant migration reaches consistent
         * state.
         */
        SemiFuture<void> _getDataConsistentFuture();

        /*
         * Wait for the data cloned via logical cloner to be consistent.
         */
        SemiFuture<TenantOplogApplier::OpTimePair> _waitForDataToBecomeConsistent();

        /*
         * Transitions the instance state to 'kLearnedFilenames'.
         */
        SemiFuture<void> _enterLearnedFilenamesState();

        /*
         * Transitions the instance state to 'kConsistent'.
         */
        SemiFuture<void> _enterConsistentState();

        /*
         * Persists the instance state doc and waits for it to be majority replicated.
         * Throws an user assertion on failure.
         */
        SemiFuture<void> _persistConsistentState();

        /*
         * Cancels the tenant migration recipient instance task work.
         */
        void _cancelRemainingWork(WithLock lk);

        /*
         * Performs some cleanup work on sync completion, like, shutting down the components or
         * fulfilling any data-sync related instance promises.
         */
        void _cleanupOnDataSyncCompletion(Status status);

        /*
         * Suppresses selecting 'host' as the donor sync source, until 'until'.
         */
        void _excludeDonorHost(WithLock, const HostAndPort& host, Date_t until);

        /*
         * Returns a vector of currently excluded donor hosts. Also removes hosts from the list of
         * excluded donor nodes, if the exclude duration has expired.
         */
        std::vector<HostAndPort> _getExcludedDonorHosts(WithLock);

        /*
         * Makes the failpoint stop or hang the migration based on failpoint data "action" field.
         * If "action" is "hang" and 'opCtx' is not null, the failpoint will be interruptible.
         */
        void _stopOrHangOnFailPoint(FailPoint* fp, OperationContext* opCtx = nullptr);

        /**
         * Updates the state doc in the database and waits for that to be propagated to a majority.
         */
        SemiFuture<void> _updateStateDocForMajority(WithLock lk) const;

        /*
         * Returns the majority OpTime on the donor node that 'client' is connected to.
         */
        OpTime _getDonorMajorityOpTime(std::unique_ptr<mongo::DBClientConnection>& client);

        /*
         * Detects recipient FCV changes during migration.
         */
        SemiFuture<void> _checkIfFcvHasChangedSinceLastAttempt();

        /**
         * Enforces that the donor and recipient share the same featureCompatibilityVersion.
         */
        void _compareRecipientAndDonorFCV() const;

        /*
         * Sets up internal state to begin migration.
         */
        void _setup();

        SemiFuture<TenantOplogApplier::OpTimePair> _migrateUsingMTMProtocol(
            const CancellationToken& token);

        SemiFuture<TenantOplogApplier::OpTimePair> _migrateUsingShardMergeProtocol(
            const CancellationToken& token);

        mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationRecipientService::_mutex");

        // All member variables are labeled with one of the following codes indicating the
        // synchronization rules for accessing them.
        //
        // (R)  Read-only in concurrent operation; no synchronization required.
        // (S)  Self-synchronizing; access according to class's own rules.
        // (M)  Reads and writes guarded by _mutex.
        // (W)  Synchronization required only for writes.

        ServiceContext* const _serviceContext;
        const TenantMigrationRecipientService* const _recipientService;  // (R) (not owned)
        std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;   // (M)
        TenantMigrationRecipientDocument _stateDoc;                      // (M)

        // This data is provided in the initial state doc and never changes.  We keep copies to
        // avoid having to obtain the mutex to access them.
        const std::string _tenantId;                                                     // (R)
        const MigrationProtocolEnum _protocol;                                           // (R)
        const UUID _migrationUuid;                                                       // (R)
        const std::string _donorConnectionString;                                        // (R)
        const MongoURI _donorUri;                                                        // (R)
        const ReadPreferenceSetting _readPreference;                                     // (R)
        const boost::optional<TenantMigrationPEMPayload> _recipientCertificateForDonor;  // (R)
        // TODO (SERVER-54085): Remove server parameter tenantMigrationDisableX509Auth.
        // Transient SSL params created based on the state doc if the server parameter
        // 'tenantMigrationDisableX509Auth' is false.
        const boost::optional<TransientSSLParams> _transientSSLParams = boost::none;  // (R)

        std::shared_ptr<ReplicaSetMonitor> _donorReplicaSetMonitor;  // (M)

        // Members of the donor replica set that we have excluded as a potential sync source for
        // some period of time.
        std::vector<std::pair<HostAndPort, Date_t>> _excludedDonorHosts;  // (M)

        // Because the cloners and oplog fetcher use exhaust, we need a separate connection for
        // each.  The '_client' will be used for the cloners and other operations such as fetching
        // optimes while the '_oplogFetcherClient' will be reserved for the oplog fetcher only.
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
        std::unique_ptr<TenantAllDatabaseCloner> _tenantAllDatabaseCloner;          // (M)
        std::shared_ptr<TenantOplogApplier> _tenantOplogApplier;                    // (M)

        // Writer pool to do storage write operation. Used by tenant collection cloner and by
        // tenant oplog applier.
        std::unique_ptr<ThreadPool> _writerPool;  //(M)
        // Data shared by cloners. Follow TenantMigrationSharedData synchronization rules.
        std::unique_ptr<TenantMigrationSharedData> _sharedData;  // (S)
        // Indicates whether the main task future continuation chain state kicked off by run().
        TaskState _taskState;  // (M)

        // Promise that is resolved when the state document is initialized and persisted.
        SharedPromise<void> _stateDocPersistedPromise;  // (W)
        // Promise that is resolved Signaled when the instance has started tenant database cloner
        // and tenant oplog fetcher.
        SharedPromise<void> _dataSyncStartedPromise;  // (W)
        // Promise that is resolved when all recipient nodes have imported all donor files.
        SharedPromise<void> _importedFilesPromise;  // (W)
        // Whether we are waiting for members to import donor files.
        bool _waitingForMembersToImportFiles = true;
        // Which members have imported all donor files.
        stdx::unordered_set<HostAndPort> _membersWhoHaveImportedFiles;
        // Promise that is resolved when the tenant data sync has reached consistent point.
        SharedPromise<OpTime> _dataConsistentPromise;  // (W)
        // Promise that is resolved when the data sync has completed.
        SharedPromise<void> _dataSyncCompletionPromise;  // (W)
        // Promise that is resolved when the recipientForgetMigration command is received or on
        // stepDown/shutDown with errors.
        SharedPromise<void> _receivedRecipientForgetMigrationPromise;  // (W)
        // Promise that is resolved when the instance has been durably marked garbage collectable
        SharedPromise<void> _forgetMigrationDurablePromise;  // (W)
        // Waiters are notified when 'tenantOplogApplier' is valid on restart.
        stdx::condition_variable _restartOplogApplierCondVar;  // (M)
        // Waiters are notified when 'tenantOplogApplier' is ready to use.
        stdx::condition_variable _oplogApplierReadyCondVar;  // (M)
        // Indicates whether 'tenantOplogApplier' is ready to use or not.
        bool _oplogApplierReady = false;  // (M)
    };

private:
    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override;

    ServiceContext* const _serviceContext;

    /*
     * Ensures that only one Instance is able to insert the initial state doc provided by the user,
     * into NamespaceString::kTenantMigrationRecipientsNamespace collection at a time.
     *
     * No other locks should be held when locking this. RSTl/global/db/collection locks have to be
     * taken after taking this.
     */
    Lock::ResourceMutex _stateDocInsertMutex{"TenantMigrationRecipientStateDocInsert::mutex"};
};
}  // namespace repl
}  // namespace mongo
