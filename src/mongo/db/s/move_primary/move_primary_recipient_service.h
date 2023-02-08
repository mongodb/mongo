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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * MovePrimaryRecipientService coordinates online movePrimary data migration on the
 * recipient side.
 */
class MovePrimaryRecipientService final : public repl::PrimaryOnlyService {
    // Disallows copying.
    MovePrimaryRecipientService(const MovePrimaryRecipientService&) = delete;
    MovePrimaryRecipientService& operator=(const MovePrimaryRecipientService&) = delete;

public:
    static constexpr StringData kMovePrimaryRecipientServiceName = "MovePrimaryRecipientService"_sd;

    explicit MovePrimaryRecipientService(ServiceContext* serviceContext);
    ~MovePrimaryRecipientService() = default;

    StringData getServiceName() const final;

    NamespaceString getStateDocumentsNS() const final;

    ThreadPool::Limits getThreadPoolLimits() const final;

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialStateDoc,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) final;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialStateDoc) final;

    class MovePrimaryRecipient final
        : public PrimaryOnlyService::TypedInstance<MovePrimaryRecipient> {
    public:
        explicit MovePrimaryRecipient(const MovePrimaryRecipientService* recipientService,
                                      MovePrimaryRecipientDocument recipientDoc,
                                      ServiceContext* serviceContext);

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancellationToken& token) noexcept final;

        /*
         * Interrupts the running instance and cause the completion future to complete with
         * 'status'.
         */
        void interrupt(Status status) override;

        /*
         * Blocks the thread until the movePrimary operation reaches consistent state in an
         * interruptible mode. Throws exception on error.
         */
        void waitUntilMigrationReachesConsistentState(OperationContext* opCtx) const;

        /*
         * Blocks the thread until the oplog applier applies the data past the
         * 'returnAfterReachingTimestamp' in an interruptible mode. If the recipient's logical clock
         * has not yet reached the 'returnAfterReachingTimestamp', advances the recipient's logical
         * clock to 'returnAfterReachingTimestamp'.
         */
        void waitUntilMigrationReachesReturnAfterReachingDonorTimestamp(
            OperationContext* opCtx, const Timestamp& returnAfterReachingTimestamp);

        /**
         * Interrupts the migration for garbage collection.
         */
        void onReceiveRecipientForgetMigration(OperationContext* opCtx);

        /**
         * Aborts the movePrimary operation at the recipient.
         */
        void onRecieveRecipientAbortMigration(OperationContext* opCtx);

        /**
         * Returns a Future that will be resolved when the instance reaches kDone state.
         */
        SharedSemiFuture<void> getForgetMigrationDurableFuture() const {
            return _forgetMigrationDurablePromise.getFuture();
        }

        /**
         * Returns a Future that will be resolved when the instance reaches kAborted state.
         */
        SharedSemiFuture<void> getAbortMigrationDurableFuture() const {
            return _abortMigrationDurablePromise.getFuture();
        }

        /**
         * Report MovePrimaryRecipientService Instances in currentOp().
         */
        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final;

        void checkIfOptionsConflict(const BSONObj& stateDoc) const final;

    private:
        // Promise that is resolved when the instance reaches kDone state
        SharedPromise<void> _forgetMigrationDurablePromise;  // (W)
        // Promise that is resolved when the instance reaches kAborted state
        SharedPromise<void> _abortMigrationDurablePromise;  // (W)
    };
};

}  // namespace mongo
