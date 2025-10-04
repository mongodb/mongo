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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/version/releases.h"

#include <memory>
#include <set>
#include <stack>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

ShardingDDLCoordinatorMetadata extractShardingDDLCoordinatorMetadata(const BSONObj& coorDoc);

class ShardingDDLCoordinator
    : public repl::PrimaryOnlyService::TypedInstance<ShardingDDLCoordinator> {
public:
    explicit ShardingDDLCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& coorDoc);

    ~ShardingDDLCoordinator() override;

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

    DDLCoordinatorTypeEnum operationType() const {
        return _coordId.getOperationType();
    }

    const ForwardableOperationMetadata& getForwardableOpMetadata() const {
        tassert(10644500, "Expected _forwardableOpMetadata to be set", _forwardableOpMetadata);
        return _forwardableOpMetadata.get();
    }

    // TODO SERVER-99655: update once the operationFCV is always present for sharded DDLs
    boost::optional<multiversion::FeatureCompatibilityVersion> getOperationFCV() const {
        const auto versionContext = getForwardableOpMetadata().getVersionContext();
        tassert(10644501,
                "Expected either no versionContext, or a versionContext with an operation FCV",
                !versionContext || versionContext->getOperationFCV(VersionContext::Passkey()));
        return versionContext
            ? boost::make_optional(
                  versionContext->getOperationFCV(VersionContext::Passkey())->getVersion())
            : boost::none;
    }

    const boost::optional<mongo::DatabaseVersion>& getDatabaseVersion() const& {
        return _databaseVersion;
    }

protected:
    const NamespaceString& originalNss() const {
        return _coordId.getNss();
    }

    virtual const NamespaceString& nss() const {
        if (const auto& bucketNss = metadata().getBucketNss()) {
            return bucketNss.get();
        }
        return originalNss();
    }

    virtual std::set<NamespaceString> _getAdditionalLocksToAcquire(OperationContext* opCtx) {
        return {};
    };

    virtual ShardingDDLCoordinatorMetadata const& metadata() const = 0;
    virtual void setMetadata(ShardingDDLCoordinatorMetadata&& metadata) = 0;

    /**
     * Returns a set of basic coordinator attributes to be used for logging.
     */
    logv2::DynamicAttributes getBasicCoordinatorAttrs() const;

    /**
     * Returns the set of attributes to be used for coordinator logging. Implementations must be
     * sure to return a DynamicAttributes object that starts with the attributes returned by
     * getBasicCoordinatorAttrs().
     */
    virtual logv2::DynamicAttributes getCoordinatorLogAttrs() const {
        return getBasicCoordinatorAttrs();
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

    ShardingDDLCoordinatorExternalState* _getExternalState();

    /**
     * Create an `OperationContext` with the `ForwardableOperationMetadata` from the coordinator
     * document set on it. Use this instead of `cc().makeOperationContext()`.
     */
    ServiceContext::UniqueOperationContext makeOperationContext() {
        auto opCtxHolder = cc().makeOperationContext();
        getForwardableOpMetadata().setOn(opCtxHolder.get());
        return opCtxHolder;
    }

    ShardingDDLCoordinatorService* _service;
    const ShardingDDLCoordinatorId _coordId;

    const bool _recoveredFromDisk;
    const boost::optional<mongo::ForwardableOperationMetadata> _forwardableOpMetadata;
    const boost::optional<mongo::DatabaseVersion> _databaseVersion;
    boost::optional<NamespaceString> _bucketNss;

    bool _firstExecution{
        true};  // True only when executing the coordinator for the first time (meaning it's not a
                // retry after a retryable error nor a recovered instance from a previous primary)
    bool _completeOnError{false};

private:
    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept final;

    virtual ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) noexcept = 0;

    virtual ExecutorFuture<void> _cleanupOnAbort(
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& token,
        const Status& status) noexcept;

    void interrupt(Status status) final;

    bool _removeDocument(OperationContext* opCtx);

    ExecutorFuture<bool> _removeDocumentUntillSuccessOrStepdown(
        std::shared_ptr<executor::TaskExecutor> executor);

    ExecutorFuture<void> _acquireAllLocksAsync(
        OperationContext* opCtx,
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& token);

    template <typename T>
    ExecutorFuture<void> _acquireLockAsync(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                           const CancellationToken& token,
                                           const T& resource,
                                           LockMode lockMode);

    ExecutorFuture<void> _translateTimeseriesNss(
        std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token);

    virtual boost::optional<Status> getAbortReason() const;

    stdx::mutex _mutex;
    SharedPromise<void> _constructionCompletionPromise;
    SharedPromise<void> _completionPromise;

    // A Locker object works attached to an opCtx and it's destroyed once the opCtx gets out of
    // scope. However, we must keep alive a unique Locker object during the whole
    // ShardingDDLCoordinator life to preserve the lock state among all the executor tasks.
    std::unique_ptr<Locker> _locker;

    std::stack<DDLLockManager::ScopedBaseDDLLock> _scopedLocks;
    std::shared_ptr<ShardingDDLCoordinatorExternalState> _externalState;

    friend class ShardingDDLCoordinatorTest;
};

template <class StateDoc>
class ShardingDDLCoordinatorImpl : public ShardingDDLCoordinator {
public:
    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override {
        return basicReportBuilder().obj();
    }

protected:
    ShardingDDLCoordinatorImpl(ShardingDDLCoordinatorService* service,
                               const std::string& name,
                               const BSONObj& initialStateDoc)
        : ShardingDDLCoordinator(service, initialStateDoc),
          _coordinatorName(name),
          /*
           * Force a deserialisation + serialisation of the initialStateDoc to ensure that
           * _initialState is a full deep copy of the received parameter.
           */
          _initialState(
              StateDoc::parse(initialStateDoc, IDLParserContext("CoordinatorInitialState"))
                  .toBSON()),
          _doc(StateDoc::parse(_initialState, IDLParserContext("CoordinatorDocument"))) {}

    ShardingDDLCoordinatorMetadata const& metadata() const override {
        return _doc.getShardingDDLCoordinatorMetadata();
    }

    void setMetadata(ShardingDDLCoordinatorMetadata&& metadata) override {
        stdx::lock_guard lk{_docMutex};
        _doc.setShardingDDLCoordinatorMetadata(std::move(metadata));
    }

    virtual void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {};

    virtual BSONObjBuilder basicReportBuilder() const noexcept {
        BSONObjBuilder bob;

        // Append static info
        bob.append("type", "op");
        bob.append(
            "ns",
            NamespaceStringUtil::serialize(originalNss(), SerializationContext::stateDefault()));
        bob.append("desc", _coordinatorName);
        bob.append("op", "command");
        bob.append("active", true);

        // Append dynamic fields from the state doc
        {
            stdx::lock_guard lk{_docMutex};
            if (const auto& bucketNss = _doc.getBucketNss()) {
                // Bucket namespace is only present in case the collection is a sharded timeseries
                bob.append("bucketNamespace",
                           NamespaceStringUtil::serialize(bucketNss.get(),
                                                          SerializationContext::stateDefault()));
            }
        }

        // Create command description
        BSONObjBuilder cmdInfoBuilder;
        {
            stdx::lock_guard lk{_docMutex};
            if (const auto& optComment = getForwardableOpMetadata().getComment()) {
                cmdInfoBuilder.append(optComment.get().firstElement());
            }
        }
        appendCommandInfo(&cmdInfoBuilder);
        bob.append("command", cmdInfoBuilder.obj());

        return bob;
    }

    const std::string _coordinatorName;
    const BSONObj _initialState;
    mutable stdx::mutex _docMutex;
    StateDoc _doc;
};

template <class StateDoc, class Phase>
class RecoverableShardingDDLCoordinator : public ShardingDDLCoordinatorImpl<StateDoc> {
protected:
    using ShardingDDLCoordinatorImpl<StateDoc>::_doc;
    using ShardingDDLCoordinatorImpl<StateDoc>::_docMutex;

    RecoverableShardingDDLCoordinator(ShardingDDLCoordinatorService* service,
                                      const std::string& name,
                                      const BSONObj& initialStateDoc)
        : ShardingDDLCoordinatorImpl<StateDoc>(service, name, initialStateDoc) {}

    virtual StringData serializePhase(const Phase& phase) const = 0;

    std::function<void()> _buildPhaseHandler(const Phase& newPhase,
                                             std::function<void(OperationContext*)>&& handlerFn) {
        return _buildPhaseHandler(
            newPhase, [](OperationContext*) { return true; }, std::move(handlerFn));
    }

    std::function<void()> _buildPhaseHandler(const Phase& newPhase,
                                             std::function<bool(OperationContext*)>&& shouldExecute,
                                             std::function<void(OperationContext*)>&& handlerFn) {
        return [=, this] {
            const auto currPhase = _getDoc().getPhase();

            if (currPhase > newPhase) {
                // Do not execute this phase if we already reached a subsequent one.
                return;
            }

            auto opCtxHolder = this->makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            if (!shouldExecute(opCtx)) {
                // Do not execute the phase if the passed in condition is not met.
                return;
            }

            if (currPhase < newPhase) {
                // Persist the new phase if this is the first time we are executing it.
                _enterPhase(newPhase);
            }

            return handlerFn(opCtx);
        };
    }

    auto _getDoc() const {
        stdx::lock_guard lk{_docMutex};
        return _doc;
    }

    virtual void _enterPhase(const Phase& newPhase) {
        auto newDoc = _getDoc();

        newDoc.setPhase(newPhase);

        LOGV2_DEBUG(5390501,
                    2,
                    "DDL coordinator phase transition",
                    "coordinatorId"_attr = _doc.getId(),
                    "newPhase"_attr = serializePhase(newDoc.getPhase()),
                    "oldPhase"_attr = serializePhase(_doc.getPhase()));

        ServiceContext::UniqueOperationContext uniqueOpCtx;
        auto opCtx = cc().getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = this->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }

        if (_doc.getPhase() == Phase::kUnset) {
            _insertStateDocument(opCtx, std::move(newDoc));
        } else {
            _updateStateDocument(opCtx, std::move(newDoc));
        }
    }

    BSONObjBuilder basicReportBuilder() const noexcept override {
        auto baseReportBuilder = ShardingDDLCoordinatorImpl<StateDoc>::basicReportBuilder();

        const auto currPhase = [&]() {
            stdx::lock_guard l{_docMutex};
            return _doc.getPhase();
        }();

        baseReportBuilder.append("currentPhase", serializePhase(currPhase));
        return baseReportBuilder;
    }

    void _insertStateDocument(OperationContext* opCtx, StateDoc&& newDoc) {
        auto copyMetadata = newDoc.getShardingDDLCoordinatorMetadata();
        copyMetadata.setRecoveredFromDisk(true);
        newDoc.setShardingDDLCoordinatorMetadata(copyMetadata);

        PersistentTaskStore<StateDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
        try {
            store.add(opCtx, newDoc, defaultMajorityWriteConcern());
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
            // A series of step-up and step-down events can cause a node to try and insert the
            // document when it has already been persisted locally, but we must still wait for
            // majority commit.
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
            WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajorityForWrite(lastLocalOpTime, opCtx->getCancellationToken())
                .get(opCtx);
        }

        {
            stdx::lock_guard lk{_docMutex};
            _doc = std::move(newDoc);
        }
    }

    void _updateStateDocument(OperationContext* opCtx, StateDoc&& newDoc) {
        PersistentTaskStore<StateDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
        tassert(10644540,
                "Expected recoveredFromDisk to be set on the coordinator document metadata",
                newDoc.getShardingDDLCoordinatorMetadata().getRecoveredFromDisk());
        store.update(opCtx,
                     BSON(StateDoc::kIdFieldName << newDoc.getId().toBSON()),
                     newDoc.toBSON(),
                     defaultMajorityWriteConcern());

        {
            stdx::lock_guard lk{_docMutex};
            _doc = std::move(newDoc);
        }
    }

    /**
     * Advances and persists the `txnNumber` to ensure causality between requests, then returns the
     * updated operation session information (OSI).
     * This modifies the _doc with a std::move, so any reference to members of the _doc will be
     * invalidated after this call
     */
    OperationSessionInfo getNewSession(OperationContext* opCtx) {
        _updateSession(opCtx);
        return getCurrentSession();
    }

    boost::optional<Status> getAbortReason() const override {
        const auto& status = _doc.getAbortReason();
        tassert(10644541, "when persisted, status must be an error", !status || !status->isOK());
        return status;
    }

    /**
     * Persists the abort reason and throws it as an exception. This causes the coordinator to fail,
     * and triggers the cleanup future chain since there is a the persisted reason.
     */
    void triggerCleanup(OperationContext* opCtx, const Status& status) {
        LOGV2_INFO(7418502,
                   "Coordinator failed, persisting abort reason",
                   "coordinatorId"_attr = _doc.getId(),
                   "phase"_attr = serializePhase(_doc.getPhase()),
                   "reason"_attr = redact(status));

        auto newDoc = _getDoc();

        auto coordinatorMetadata = newDoc.getShardingDDLCoordinatorMetadata();

        coordinatorMetadata.setAbortReason(sharding_ddl_util::possiblyTruncateErrorStatus(status));
        newDoc.setShardingDDLCoordinatorMetadata(std::move(coordinatorMetadata));

        _updateStateDocument(opCtx, std::move(newDoc));

        uassertStatusOK(status);
    }

private:
    // lazily acquire Logical Session ID and a txn number
    void _updateSession(OperationContext* opCtx) {
        auto newDoc = _getDoc();
        auto newShardingDDLCoordinatorMetadata = newDoc.getShardingDDLCoordinatorMetadata();

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

        newDoc.setShardingDDLCoordinatorMetadata(std::move(newShardingDDLCoordinatorMetadata));
        _updateStateDocument(opCtx, std::move(newDoc));
    }

    OperationSessionInfo getCurrentSession() const {
        auto optSession = [&] {
            stdx::lock_guard lk{_docMutex};
            return _doc.getShardingDDLCoordinatorMetadata().getSession();
        }();

        tassert(10644542,
                "Expected session to be set on the coordinator document metadata",
                optSession);

        OperationSessionInfo osi;
        osi.setSessionId(optSession->getLsid());
        osi.setTxnNumber(optSession->getTxnNumber());
        return osi;
    }
};

#undef MONGO_LOGV2_DEFAULT_COMPONENT

}  // namespace mongo
