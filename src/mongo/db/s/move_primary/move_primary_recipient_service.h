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

#include "mongo/base/string_data.h"
#include "mongo/db/s/move_primary/move_primary_recipient_cmds_gen.h"
#include <boost/optional.hpp>
#include <memory>

#include "mongo/client/fetcher.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/time_support.h"

namespace mongo {

class DBClientConnection;
class OperationContext;
class ReplicaSetMonitor;
class ServiceContext;

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

    NamespaceString getStateDocumentsNS() const final override {
        return NamespaceString::kMovePrimaryRecipientNamespace;
    }

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

        /**
         * Returns a Future that will be resolved when the instance reaches kStarted state.
         */
        SharedSemiFuture<void> getRecipientDocDurableFuture() const {
            return _recipientDocDurablePromise.getFuture();
        }

        /**
         * Report MovePrimaryRecipientService Instances in currentOp().
         */
        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final;

        void checkIfOptionsConflict(const BSONObj& stateDoc) const final;

        StringData getDatabaseName() const;

        UUID getMigrationId() const;

    private:
        ExecutorFuture<void> _enterStartedState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
            const CancellationToken& token);

        ExecutorFuture<void> _persistRecipientDoc(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
            const CancellationToken& token);

        StringData _parseRecipientState(MovePrimaryRecipientState value);

        const NamespaceString _stateDocumentNS = NamespaceString::kMovePrimaryRecipientNamespace;

        const MovePrimaryRecipientService* _recipientService;

        const MovePrimaryRecipientMetadata _metadata;

        ServiceContext* _serviceContext;

        // To synchronize operations on mutable states below
        Mutex _mutex = MONGO_MAKE_LATCH("MovePrimaryRecipient::_mutex");

        MovePrimaryRecipientState _state;

        // Promise that is resolved when the recipient doc is persisted in kStarted state
        SharedPromise<void> _recipientDocDurablePromise;
    };

private:
    ServiceContext* const _serviceContext;
};

}  // namespace mongo
