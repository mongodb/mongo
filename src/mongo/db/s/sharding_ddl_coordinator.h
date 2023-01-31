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

#include "mongo/db/internal_session_pool.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"

namespace mongo {

ShardingDDLCoordinatorMetadata extractShardingDDLCoordinatorMetadata(const BSONObj& coorDoc);

class ShardingDDLCoordinator
    : public repl::PrimaryOnlyService::TypedInstance<ShardingDDLCoordinator> {
public:
    explicit ShardingDDLCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& coorDoc);

    ~ShardingDDLCoordinator();

    /*
     * Check if the given coordinator document has the same options as this.
     *
     * This is used to decide if we can join a previously created coordinator.
     * In the case the given coordinator document has incompatible options with this,
     * this function must throw a ConflictingOperationInProgress exception with an adequate message.
     */
    virtual void checkIfOptionsConflict(const BSONObj& coorDoc) const = 0;

    /**
     * Whether this coordinator is allowed to start when user write blocking is enabled, even if the
     * writeBlockingBypass flag is not set. Coordinators that don't affect user data shall always be
     * allowed to run even when user write blocking is enabled.
     */
    virtual bool canAlwaysStartWhenUserWritesAreDisabled() const {
        return false;
    }

    /*
     * Returns a future that will be completed when the construction of this coordinator instance
     * is completed.
     *
     * In particular the returned future will be ready only after this coordinator succesfully
     * aquires the required locks.
     */
    SharedSemiFuture<void> getConstructionCompletionFuture() {
        return _constructionCompletionPromise.getFuture();
    }

    /*
     * Returns a future that will be ready when all the work associated with this coordinator
     * isntances will be completed.
     *
     * In particular the returned future will be ready after this coordinator will succesfully
     * release all the aquired locks.
     */
    SharedSemiFuture<void> getCompletionFuture() {
        return _completionPromise.getFuture();
    }

    const NamespaceString& nss() const {
        return _coordId.getNss();
    }

    DDLCoordinatorTypeEnum operationType() const {
        return _coordId.getOperationType();
    }

    const ForwardableOperationMetadata& getForwardableOpMetadata() const {
        invariant(metadata().getForwardableOpMetadata());
        return metadata().getForwardableOpMetadata().get();
    }

    const boost::optional<mongo::DatabaseVersion>& getDatabaseVersion() const& {
        return metadata().getDatabaseVersion();
    }

protected:
    virtual std::vector<StringData> _acquireAdditionalLocks(OperationContext* opCtx) {
        return {};
    };

    virtual ShardingDDLCoordinatorMetadata const& metadata() const = 0;

    template <typename StateDoc>
    StateDoc _insertStateDocument(StateDoc&& newDoc) {
        auto copyMetadata = newDoc.getShardingDDLCoordinatorMetadata();
        copyMetadata.setRecoveredFromDisk(true);
        newDoc.setShardingDDLCoordinatorMetadata(copyMetadata);

        auto opCtx = cc().makeOperationContext();
        PersistentTaskStore<StateDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
        try {
            store.add(opCtx.get(), newDoc, WriteConcerns::kMajorityWriteConcernNoTimeout);
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
            // A series of step-up and step-down events can cause a node to try and insert the
            // document when it has already been persisted locally, but we must still wait for
            // majority commit.
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
            const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
            WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(lastLocalOpTime, opCtx.get()->getCancellationToken())
                .get(opCtx.get());
        }

        return std::move(newDoc);
    }

    template <typename StateDoc>
    StateDoc _updateStateDocument(OperationContext* opCtx, StateDoc&& newDoc) {
        PersistentTaskStore<StateDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
        invariant(newDoc.getShardingDDLCoordinatorMetadata().getRecoveredFromDisk());
        store.update(opCtx,
                     BSON(StateDoc::kIdFieldName << newDoc.getId().toBSON()),
                     newDoc.toBSON(),
                     WriteConcerns::kMajorityWriteConcernNoTimeout);
        return std::move(newDoc);
    }

    // lazily acqiure Logical Session ID and a txn number
    template <typename StateDoc>
    StateDoc _updateSession(OperationContext* opCtx, StateDoc const& doc) {
        auto newShardingDDLCoordinatorMetadata = doc.getShardingDDLCoordinatorMetadata();

        auto optSession = newShardingDDLCoordinatorMetadata.getSession();
        if (optSession) {
            auto txnNumber = optSession->getTxnNumber();
            optSession->setTxnNumber(++txnNumber);
            newShardingDDLCoordinatorMetadata.setSession(optSession);
        } else {
            auto session = InternalSessionPool::get(opCtx)->acquireSystemSession();
            newShardingDDLCoordinatorMetadata.setSession(
                ShardingDDLSession(session.getSessionId(), session.getTxnNumber()));
        }

        StateDoc newDoc(doc);
        newDoc.setShardingDDLCoordinatorMetadata(std::move(newShardingDDLCoordinatorMetadata));
        return _updateStateDocument(opCtx, std::move(newDoc));
    }

    template <typename StateDoc>
    OperationSessionInfo getCurrentSession(StateDoc const& doc) const {
        invariant(doc.getShardingDDLCoordinatorMetadata().getSession());
        ShardingDDLSession shardingDDLSession =
            *doc.getShardingDDLCoordinatorMetadata().getSession();

        OperationSessionInfo osi;
        osi.setSessionId(shardingDDLSession.getLsid());
        osi.setTxnNumber(shardingDDLSession.getTxnNumber());
        return osi;
    }

    /*
     * Performs a noop write on all shards and the configsvr using the sessionId and txnNumber
     * specified in 'osi'.
     */
    void _performNoopRetryableWriteOnAllShardsAndConfigsvr(
        OperationContext* opCtx,
        const OperationSessionInfo& osi,
        const std::shared_ptr<executor::TaskExecutor>& executor);

    /*
     * Specify if the coordinator must indefinitely be retried in case of exceptions. It is always
     * expected for a coordinator to make progress after performing intermediate operations that
     * can't be rollbacked.
     */
    virtual bool _mustAlwaysMakeProgress() {
        return false;
    };

    /*
     * Specify if the given error will be retried by the ddl coordinator infrastructure.
     */
    bool _isRetriableErrorForDDLCoordinator(const Status& status);

    ShardingDDLCoordinatorService* _service;
    const ShardingDDLCoordinatorId _coordId;

    const bool _recoveredFromDisk;
    bool _firstExecution{
        true};  // True only when executing the coordinator for the first time (meaning it's not a
                // retry after a retryable error nor a recovered instance from a previous primary)
    bool _completeOnError{false};

private:
    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override final;

    virtual ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) noexcept = 0;

    void interrupt(Status status) override final;

    bool _removeDocument(OperationContext* opCtx);

    ExecutorFuture<bool> _removeDocumentUntillSuccessOrStepdown(
        std::shared_ptr<executor::TaskExecutor> executor);

    ExecutorFuture<void> _acquireLockAsync(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                           const CancellationToken& token,
                                           StringData resource);

    Mutex _mutex = MONGO_MAKE_LATCH("ShardingDDLCoordinator::_mutex");
    SharedPromise<void> _constructionCompletionPromise;
    SharedPromise<void> _completionPromise;

    std::stack<DistLockManager::ScopedLock> _scopedLocks;
};

}  // namespace mongo
