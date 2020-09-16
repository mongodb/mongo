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

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"

namespace mongo {

class DBClientConnection;
class OperationContext;
class ReplicaSetMonitor;
class ServiceContext;

namespace repl {

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

        /*
         *  Returns the instance id.
         */
        const UUID& getMigrationUUID() const;

        /*
         *  Returns the tenant id (database prefix).
         */
        const std::string& getTenantId() const;

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
    };
};


}  // namespace repl
}  // namespace mongo
