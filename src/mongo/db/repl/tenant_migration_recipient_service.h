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

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialStateDoc) const final;

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        explicit Instance(const TenantMigrationRecipientService* recipientService,
                          BSONObj stateDoc);

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancelationToken& token) noexcept final;

        /*
         * Interrupts the running instance and cause the completion future to complete with
         * 'status'.
         */
        void interrupt(Status status) override;

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
         * Returns a Future that will be resolved when the work associated with this Instance has
         * completed to indicate whether the migration is forgotten successfully.
         */
        SharedSemiFuture<void> getCompletionFuture() const {
            return _taskCompletionPromise.getFuture();
        }

        /**
         * TODO(SERVER-50974) Report TenantMigrationRecipientService Instances in currentOp().
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

        /**
         * To be called on the instance returned by PrimaryOnlyService::getOrCreate(). Returns an
         * error if the options this Instance was created with are incompatible with a request for
         * an instance with the options given in 'options'.
         */
        Status checkIfOptionsConflict(const TenantMigrationRecipientDocument& StateDoc) const;

        /*
         * Blocks the thread until the tenant migration reaches consistent state in an interruptible
         * mode. Returns the donor optime at which the migration reached consistent state. Throws
         * exception on error.
         */
        OpTime waitUntilMigrationReachesConsistentState(OperationContext* opCtx) const;

        /*
         * Blocks the thread until the tenant oplog applier applied data past the given 'donorTs'
         * in an interruptible mode. Returns the majority applied donor optime which may be greater
         * or equal to given 'donorTs'. Throws exception on error.
         */
        OpTime waitUntilTimestampIsMajorityCommitted(OperationContext* opCtx,
                                                     const Timestamp& donorTs) const;

        /*
         * Suppresses selecting 'host' as the donor sync source, until 'until'.
         */
        void excludeDonorHost(const HostAndPort& host, Date_t until);

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
                        return newState == kDone;
                    case kDone:
                        return false;
                }
                MONGO_UNREACHABLE;
            }

            void setState(StateFlag state, boost::optional<Status> interruptStatus = boost::none) {
                invariant(checkIfValidTransition(state),
                          str::stream() << "current state: " << toString(_state)
                                        << ", new state: " << toString(state));

                _state = state;
                if (interruptStatus) {
                    invariant(_state == kInterrupted && !interruptStatus->isOK());
                    _interruptStatus = interruptStatus.get();
                }
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
            // task interrupt status.
            Status _interruptStatus = Status{ErrorCodes::InternalError, "Uninitialized value"};
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
        SemiFuture<void> _markStateDocumentAsGarbageCollectable();

        /**
         * Creates a client, connects it to the donor, and authenticates it if authParams is
         * non-empty.  Throws a user assertion on failure.
         *
         */
        std::unique_ptr<DBClientConnection> _connectAndAuth(
            const HostAndPort& serverAddress,
            StringData applicationName,
            const TransientSSLParams* transientSSLParams);

        /**
         * Creates and connects both the oplog fetcher client and the client used for other
         * operations.
         */
        SemiFuture<ConnectionPair> _createAndConnectClients();

        /**
         * Retrieves the start optimes from the donor and updates the in-memory state accordingly.
         */
        void _getStartOpTimesFromDonor(WithLock);

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
         * Indicates that the recipient has completed the tenant cloning phase.
         */
        bool _isCloneCompletedMarkerSet(WithLock) const;

        /*
         * Starts the tenant cloner.
         * Returns future that will be fulfilled when the cloner completes.
         */
        Future<void> _startTenantAllDatabaseCloner(WithLock lk);

        /*
         * Gets called when the cloner completes cloning data successfully.
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
         * Cancels the tenant migration recipient instance task work.
         */
        void _cancelRemainingWork(WithLock lk);

        /*
         * Performs some cleanup work on sync completion, like, shutting down the components or
         * fulfilling any data-sync related instance promises.
         */
        void _cleanupOnDataSyncCompletion(Status status);

        /*
         * Returns a vector of currently excluded donor hosts. Also removes hosts from the list of
         * excluded donor nodes, if the exclude duration has expired.
         */
        std::vector<HostAndPort> _getExcludedDonorHosts(WithLock lk);

        /*
         * Makes the failpoint to stop or hang based on failpoint data "action" field.
         */
        void _stopOrHangOnFailPoint(FailPoint* fp);

        mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationRecipientService::_mutex");

        // All member variables are labeled with one of the following codes indicating the
        // synchronization rules for accessing them.
        //
        // (R)  Read-only in concurrent operation; no synchronization required.
        // (S)  Self-synchronizing; access according to class's own rules.
        // (M)  Reads and writes guarded by _mutex.
        // (W)  Synchronization required only for writes.

        const TenantMigrationRecipientService* const _recipientService;  // (R) (not owned)
        std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;   // (M)
        TenantMigrationRecipientDocument _stateDoc;                      // (M)

        // This data is provided in the initial state doc and never changes.  We keep copies to
        // avoid having to obtain the mutex to access them.
        const std::string _tenantId;                  // (R)
        const UUID _migrationUuid;                    // (R)
        const std::string _donorConnectionString;     // (R)
        const ReadPreferenceSetting _readPreference;  // (R)

        std::shared_ptr<ReplicaSetMonitor> _donorReplicaSetMonitor;  // (M)

        // Members of the donor replica set that we have excluded as a potential sync source for
        // some period of time.
        std::vector<std::pair<HostAndPort, Date_t>> _excludedDonorHosts;  // (M)

        // Because the cloners and oplog fetcher use exhaust, we need a separate connection for
        // each.  The '_client' will be used for the cloners and other operations such as fetching
        // optimes while the '_oplogFetcherClient' will be reserved for the oplog fetcher only.
        std::unique_ptr<DBClientConnection> _client;              // (M)
        std::unique_ptr<DBClientConnection> _oplogFetcherClient;  // (M)

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
        // Promise that is resolved Signaled when the tenant data sync has reached consistent point.
        SharedPromise<OpTime> _dataConsistentPromise;  // (W)
        // Promise that is resolved when the data sync has completed.
        SharedPromise<void> _dataSyncCompletionPromise;  // (W)
        // Promise that is resolved when the recipientForgetMigration command is received or on
        // stepDown/shutDown with errors.
        SharedPromise<void> _receivedRecipientForgetMigrationPromise;  // (W)
        // Promise that is resolved when the chain of work kicked off by run() has completed to
        // indicate whether the state doc is successfully marked as garbage collectable.
        SharedPromise<void> _taskCompletionPromise;  // (W)
    };

private:
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
