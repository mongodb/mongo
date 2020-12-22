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

#include "mongo/base/string_data.h"
#include "mongo/client/remote_command_targeter_rs.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_migration_donor_util.h"
#include "mongo/util/cancelation.h"
#include "mongo/util/string_map.h"

namespace mongo {

class TenantMigrationDonorService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "TenantMigrationDonorService"_sd;

    explicit TenantMigrationDonorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {
        _serviceContext = serviceContext;
    }
    ~TenantMigrationDonorService() = default;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kTenantMigrationDonorsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        ThreadPool::Limits limits;
        limits.maxThreads = repl::maxTenantMigrationDonorThreadPoolSize;
        return limits;
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) const override {
        return std::make_shared<TenantMigrationDonorService::Instance>(_serviceContext,
                                                                       initialState);
    }

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        struct DurableState {
            TenantMigrationDonorStateEnum state;
            boost::optional<Status> abortReason;
        };

        Instance(ServiceContext* serviceContext, const BSONObj& initialState);

        ~Instance();

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancelationToken& token) noexcept override;

        void interrupt(Status status) override;

        /**
         * Report TenantMigrationDonorService Instances in currentOp().
         */
        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

        /**
         * To be called on the instance returned by PrimaryOnlyService::getOrCreate. Returns an
         * error if the options this Instance was created with are incompatible with a request for
         * an instance with the options given in 'options'.
         */
        Status checkIfOptionsConflict(BSONObj options);

        /**
         * Returns the latest durable migration state.
         */
        DurableState getDurableState(OperationContext* opCtx);

        /**
         * Returns a Future that will be resolved when all work associated with this Instance has
         * completed running.
         */
        SharedSemiFuture<void> getCompletionFuture() const {
            return _completionPromise.getFuture();
        }

        void onReceiveDonorForgetMigration();

    private:
        const NamespaceString _stateDocumentsNS = NamespaceString::kTenantMigrationDonorsNamespace;

        /**
         * Inserts the state document to _stateDocumentsNS and returns the opTime for the insert
         * oplog entry.
         */
        ExecutorFuture<repl::OpTime> _insertStateDocument(
            std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancelationToken& token);

        /**
         * Updates the state document to have the given state. Then, persists the updated document
         * by reserving an oplog slot beforehand and using its timestamp as the blockTimestamp or
         * commitOrAbortTimestamp depending on the state. Returns the opTime for the update oplog
         * entry.
         */
        ExecutorFuture<repl::OpTime> _updateStateDocument(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            const TenantMigrationDonorStateEnum nextState,
            const CancelationToken& token);

        /**
         * Sets the "expireAt" time for the state document to be garbage collected, and returns the
         * the opTime for the write.
         */
        ExecutorFuture<repl::OpTime> _markStateDocumentAsGarbageCollectable(
            std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancelationToken& token);

        /**
         * Waits for given opTime to be majority committed.
         */
        ExecutorFuture<void> _waitForMajorityWriteConcern(
            std::shared_ptr<executor::ScopedTaskExecutor> executor, repl::OpTime opTime);

        /**
         * Sends the given command to the recipient replica set.
         */
        ExecutorFuture<void> _sendCommandToRecipient(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const BSONObj& cmdObj,
            const CancelationToken& token);

        /**
         * Sends the recipientSyncData command to the recipient replica set.
         */
        ExecutorFuture<void> _sendRecipientSyncDataCommand(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const CancelationToken& token);

        /**
         * Sends the recipientForgetMigration command to the recipient replica set.
         */
        ExecutorFuture<void> _sendRecipientForgetMigrationCommand(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const CancelationToken& token);

        ServiceContext* _serviceContext;

        TenantMigrationDonorDocument _stateDoc;
        boost::optional<Status> _abortReason;

        // Protects the durable state and the promises below.
        mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationDonorService::_mutex");

        // The latest majority-committed migration state.
        DurableState _durableState;

        // Promise that is resolved when the donor has majority-committed the write to insert the
        // donor state doc for the migration.
        SharedPromise<void> _initialDonorStateDurablePromise;

        // Promise that is resolved when the donor receives the donorForgetMigration command.
        SharedPromise<void> _receiveDonorForgetMigrationPromise;

        // Promise that is resolved when the chain of work kicked off by run() has completed.
        SharedPromise<void> _completionPromise;
    };

private:
    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancelationToken& token) override;

    ServiceContext* _serviceContext;
};
}  // namespace mongo
