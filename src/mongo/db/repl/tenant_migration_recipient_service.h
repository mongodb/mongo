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
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"

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

    explicit TenantMigrationRecipientService(ServiceContext* serviceContext);
    ~TenantMigrationRecipientService() = default;

    StringData getServiceName() const final;

    NamespaceString getStateDocumentsNS() const final;

    ThreadPool::Limits getThreadPoolLimits() const final;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialStateDoc) const final;

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        explicit Instance(BSONObj stateDoc);

        void run(std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept final;

        void interrupt(Status status) override;

        /**
         * Returns a Future that will be resolved when all work associated with this Instance has
         * completed running.
         */
        SharedSemiFuture<void> getCompletionFuture() const {
            return _completionPromise.getFuture();
        }

        /**
         * TODO(SERVER-50974) Report TenantMigrationRecipientService Instances in currentOp().
         */
        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final {
            return boost::none;
        }

        /*
         *  Returns the instance id.
         */
        const UUID& getMigrationUUID() const;

        /*
         *  Returns the tenant id (database prefix).
         */
        const std::string& getTenantId() const;

        /*
         *  Set the oplog creator functor, to allow use of a mock oplog fetcher.
         */
        void setCreateOplogFetcherFn_forTest(
            std::unique_ptr<OplogFetcherFactory>&& createOplogFetcherFn) {
            _createOplogFetcherFn = std::move(createOplogFetcherFn);
        }

    private:
        friend class TenantMigrationRecipientServiceTest;

        /*
         * Transitions the instance state to 'kStarted'.
         *
         * Persists the instance state doc and waits for it to be majority replicated.
         * Throws an user assertion on failure.
         */
        SharedSemiFuture<void> _initializeStateDoc();

        /**
         * Creates a client, connects it to the donor, and authenticates it if authParams is
         * non-empty.  Throws a user assertion on failure.
         *
         */
        std::unique_ptr<DBClientConnection> _connectAndAuth(const HostAndPort& serverAddress,
                                                            StringData applicationName,
                                                            BSONObj authParams);

        /**
         * Creates and connects both the oplog fetcher client and the client used for other
         * operations.
         */
        SemiFuture<void> _createAndConnectClients();

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

        std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;

        // Protects below non-const data members.
        mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationRecipientService::_mutex");

        TenantMigrationRecipientDocument _stateDoc;

        // This data is provided in the initial state doc and never changes.  We keep copies to
        // avoid having to obtain the mutex to access them.
        const std::string _tenantId;
        const UUID _migrationUuid;
        const std::string _donorConnectionString;
        const ReadPreferenceSetting _readPreference;
        // TODO(SERVER-50670): Populate authParams
        const BSONObj _authParams;
        // Promise that is resolved when the chain of work kicked off by run() has completed.
        SharedPromise<void> _completionPromise;

        std::shared_ptr<ReplicaSetMonitor> _donorReplicaSetMonitor;

        // Because the cloners and oplog fetcher use exhaust, we need a separate connection for
        // each.  The '_client' will be used for the cloners and other operations such as fetching
        // optimes while the '_oplogFetcherClient' will be reserved for the oplog fetcher only.
        std::unique_ptr<DBClientConnection> _client;
        std::unique_ptr<DBClientConnection> _oplogFetcherClient;


        std::unique_ptr<OplogFetcherFactory> _createOplogFetcherFn =
            std::make_unique<CreateOplogFetcherFn>();
        std::unique_ptr<OplogBufferCollection> _donorOplogBuffer;
        std::unique_ptr<DataReplicatorExternalState> _dataReplicatorExternalState;
        std::unique_ptr<OplogFetcher> _donorOplogFetcher;
    };
};


}  // namespace repl
}  // namespace mongo
