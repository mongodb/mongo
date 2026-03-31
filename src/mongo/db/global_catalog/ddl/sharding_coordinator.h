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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
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
#include "mongo/util/modules.h"
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

MONGO_MOD_NEEDS_REPLACEMENT ShardingCoordinatorMetadata
extractShardingCoordinatorMetadata(const BSONObj& coorDoc);

/**
 * Generic coordinator phase enum.
 * It may be converted to any enum used by coordinator implementations, provided that it has the
 * same underlying type and that it has a kUnset member with the value `0`.
 */
enum class MONGO_MOD_PRIVATE CoordinatorGenericPhase : std::int32_t {
    kUnset = 0,
};

/**
 * Represents a "generic" coordinator StateDoc.
 * This interface is implemented by the template class `CoordinatorStateDocImpl` defined below.
 */
class MONGO_MOD_PRIVATE CoordinatorStateDoc {
public:
    static constexpr auto kIdFieldName = "_id"_sd;

    virtual ~CoordinatorStateDoc() = default;
    virtual const ShardingCoordinatorMetadata& getShardingCoordinatorMetadata() const = 0;
    virtual void setShardingCoordinatorMetadata(ShardingCoordinatorMetadata newMetadata) = 0;
    virtual void replace(std::unique_ptr<CoordinatorStateDoc> newDoc) = 0;
    virtual void setGenericPhase(CoordinatorGenericPhase p) = 0;
    virtual CoordinatorGenericPhase getGenericPhase() const = 0;
    virtual BSONObj toBSON() const = 0;
    virtual std::unique_ptr<CoordinatorStateDoc> clone() const = 0;
};

class MONGO_MOD_NEEDS_REPLACEMENT ShardingCoordinator
    : public repl::PrimaryOnlyService::TypedInstance<ShardingCoordinator> {
public:
    static inline const auto kExponentialBackoff = Backoff(Seconds(1), Milliseconds::max());

    explicit ShardingCoordinator(ShardingCoordinatorService* service,
                                 std::string name,
                                 const BSONObj& coorDoc);

    ~ShardingCoordinator() override;

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
     * In particular the returned future will be ready only after this coordinator successfully
     * acquires the required locks.
     */
    SharedSemiFuture<void> getConstructionCompletionFuture() {
        return _constructionCompletionPromise.getFuture();
    }

    /*
     * Returns a future that will be ready when all the work associated with this coordinator
     * instances will be completed.
     *
     * In particular the returned future will be ready after this coordinator will successfully
     * release all the acquired locks.
     */
    SharedSemiFuture<void> getCompletionFuture() {
        return _completionPromise.getFuture();
    }

    CoordinatorTypeEnum operationType() const {
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

    virtual ShardingCoordinatorMetadata const& metadata() const {
        return getDoc().getShardingCoordinatorMetadata();
    }

    virtual void setMetadata(ShardingCoordinatorMetadata&& metadata) {
        stdx::lock_guard lk{_docMutex};
        getDoc().setShardingCoordinatorMetadata(std::move(metadata));
    }

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
     * Specify if the coordinator must indefinitely be retried in case of exceptions. It is always
     * expected for a coordinator to make progress after performing intermediate operations that
     * can't be rolled back.
     */
    virtual bool _mustAlwaysMakeProgress() {
        return false;
    }

    /*
     * Specify if the given error will be retried by the coordinator infrastructure.
     */
    bool _isRetriableErrorForDDLCoordinator(const Status& status);

    ShardingCoordinatorExternalState* _getExternalState();

    virtual bool _isInCriticalSectionGeneric(CoordinatorGenericPhase phase) const = 0;

    /**
     * Create an `OperationContext` with the `ForwardableOperationMetadata` from the coordinator
     * document set on it. Use this instead of `cc().makeOperationContext()`.
     * This version will automatically mark the `OperationContext` as non-deprioritizable if the
     * current phase is in the critical section.
     */
    ServiceContext::UniqueOperationContext makeOperationContext() {
        const auto currentPhase = getDoc().getGenericPhase();
        const auto deprioritizable = !_isInCriticalSectionGeneric(currentPhase);
        return makeOperationContext(deprioritizable);
    }

    /**
     * Create an `OperationContext` with the `ForwardableOperationMetadata` from the coordinator
     * document set on it. Use this instead of `cc().makeOperationContext()`.
     * If deprioritizable is false, then the `OperationContext` wil be marked as non-deprioritizable
     */
    ServiceContext::UniqueOperationContext makeOperationContext(bool deprioritizable) {
        auto opCtxHolder = cc().makeOperationContext();
        getForwardableOpMetadata().setOn(opCtxHolder.get());
        if (!deprioritizable) {
            ExecutionAdmissionContext::get(opCtxHolder.get())
                .setTaskType(opCtxHolder.get(),
                             ExecutionAdmissionContext::TaskType::NonDeprioritizable);
        }
        return opCtxHolder;
    }

    /**
     * Return a reference to this coordinator's CoordinatorStateDoc.
     */
    virtual const CoordinatorStateDoc& getDoc() const = 0;

    /**
     * Return a reference to this coordinator's CoordinatorStateDoc.
     */
    virtual CoordinatorStateDoc& getDoc() = 0;

    virtual void _initialize(OperationContext* opCtx) = 0;

    virtual void _checkCoordinatorPreconditions(OperationContext* opCtx,
                                                bool afterAcquiringLocks) = 0;

    virtual ExecutorFuture<void> _acquireLocksAsync(
        OperationContext* opCtx,
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& token) = 0;

    virtual void _releaseLocks(OperationContext* opCtx) = 0;

    virtual void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {}

    virtual BSONObjBuilder basicReportBuilder() const noexcept;

    const std::string _coordinatorName;
    mutable stdx::mutex _docMutex;

    ShardingCoordinatorService* _service;
    const ShardingCoordinatorId _coordId;

    const bool _recoveredFromDisk;
    const boost::optional<mongo::ForwardableOperationMetadata> _forwardableOpMetadata;
    boost::optional<NamespaceString> _bucketNss;

    bool _firstExecution{
        true};  // True only when executing the coordinator for the first time (meaning it's not a
                // retry after a retryable error nor a recovered instance from a previous primary)
    bool _completeOnError{false};

private:
    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override {
        return basicReportBuilder().obj();
    }

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept final;

    virtual ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) noexcept = 0;

    virtual ExecutorFuture<void> _cleanupOnAbort(
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& token,
        const Status& status) noexcept;

    virtual void _onCleanup(OperationContext* opCtx) {}

    void interrupt(Status status) final;

    bool _removeDocument(OperationContext* opCtx);

    ExecutorFuture<bool> _removeDocumentUntillSuccessOrStepdown(
        std::shared_ptr<executor::TaskExecutor> executor);

    ExecutorFuture<void> _translateTimeseriesNss(
        std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token);

    virtual boost::optional<Status> getAbortReason() const;

    stdx::mutex _mutex;
    SharedPromise<void> _constructionCompletionPromise;
    SharedPromise<void> _completionPromise;

    std::shared_ptr<ShardingCoordinatorExternalState> _externalState;

    friend class ShardingDDLCoordinatorMixin;
    friend class ShardingDDLCoordinatorTest;
};

class MONGO_MOD_UNFORTUNATELY_OPEN RecoverableShardingCoordinator
    : public ShardingCoordinator,
      public OperationSessionPersistence {
protected:
    RecoverableShardingCoordinator(ShardingCoordinatorService* service,
                                   const std::string& name,
                                   const BSONObj& initialStateDoc)
        : ShardingCoordinator(service, name, initialStateDoc), _sessionTracker(this) {}

    /**
     * Serialize a CoordinatorGenericPhase using the implementation's Phase.
     */
    virtual StringData serializeGenericPhase(CoordinatorGenericPhase phase) const = 0;

    /**
     * Advances and persists the txnNumber to ensure causality between requests, then returns the
     * updated operation session information (OSI).
     */
    OperationSessionInfo getNewSession(OperationContext* opCtx) {
        return _sessionTracker.getNextSession(opCtx);
    }

    /**
     * Releases the current session back to the InternalSessionPool and clears the persisted
     * session state. No-op if no session is currently held.
     */
    void releaseSession(OperationContext* opCtx) {
        _sessionTracker.releaseSession(opCtx);
    }

    /**
     * Advances the session and performs the given causality barrier, ensuring that subsequent
     * reads on the barrier's participants will reflect all prior writes.
     */
    void performCausalityBarrier(OperationContext* opCtx, CausalityBarrier& barrier) {
        _sessionTracker.performCausalityBarrier(opCtx, barrier);
    }

    std::function<void()> _buildPhaseHandlerGeneric(
        CoordinatorGenericPhase newPhase, std::function<void(OperationContext*)>&& handlerFn);

    std::function<void()> _buildPhaseHandlerGeneric(
        CoordinatorGenericPhase newPhase,
        std::function<bool(OperationContext*)>&& shouldExecute,
        std::function<void(OperationContext*)>&& handlerFn);

    auto _cloneDoc() const {
        stdx::lock_guard lk{_docMutex};
        return getDoc().clone();
    }

    void _enterPhaseGeneric(CoordinatorGenericPhase newPhase);

    BSONObjBuilder basicReportBuilder() const noexcept override;

    void _insertStateDocumentGeneric(OperationContext* opCtx,
                                     std::unique_ptr<CoordinatorStateDoc> newDoc);

    void _updateStateDocumentGeneric(OperationContext* opCtx,
                                     std::unique_ptr<CoordinatorStateDoc> newDoc);

    boost::optional<Status> getAbortReason() const override;

    /**
     * Persists the abort reason and throws it as an exception. This causes the coordinator to fail,
     * and triggers the cleanup future chain since there is a persisted reason.
     */
    void triggerCleanup(OperationContext* opCtx, const Status& status);

private:
    void _onCleanup(OperationContext* opCtx) override;

    boost::optional<OperationSessionInfo> readSession(OperationContext* opCtx) const override;

    void writeSession(OperationContext* opCtx,
                      const boost::optional<OperationSessionInfo>& osi) override;

    OperationSessionTracker _sessionTracker;
};

template <typename StateDoc>
class MONGO_MOD_PRIVATE CoordinatorStateDocImpl : public CoordinatorStateDoc {
    constexpr static bool kDocHasPhase = requires(StateDoc doc) { doc.getPhase(); };

    consteval static auto getPhaseTypeHelper() {
        if constexpr (kDocHasPhase) {
            return StateDoc{}.getPhase();
        } else {
            // For documents without getPhase (used in non-recoverable coordinators) return
            // CoordinatorGenericPhase so that the static_assert's down below keep working.
            return CoordinatorGenericPhase{};
        }
    }

public:
    using DocPhase = decltype(getPhaseTypeHelper());
    using DocPhaseUnderlying = std::underlying_type_t<DocPhase>;

    static_assert(
        std::is_same_v<DocPhaseUnderlying, std::underlying_type_t<CoordinatorGenericPhase>>,
        "The coordinator phase enumeration doesn't have the same underlying type as "
        "CoordinatorGenericPhase");
    static_assert(
        requires(DocPhase p) { p = DocPhase::kUnset; },
        "The coordinator phase enumeration doesn't have a member `kUnset`");
    static_assert(static_cast<DocPhaseUnderlying>(DocPhase::kUnset) ==
                      static_cast<DocPhaseUnderlying>(CoordinatorGenericPhase::kUnset),
                  "The coordinator phase enumeration kUnset doesn't have the same value as "
                  "CoordinatorGenericPhase::kUnset (it must be 0)");
    static_assert(kDocHasPhase || std::is_same_v<DocPhase, CoordinatorGenericPhase>,
                  "CoordinatorGenericPhase can't be the coordinator's phase enumeration");

    explicit CoordinatorStateDocImpl() = default;
    explicit CoordinatorStateDocImpl(StateDoc doc) : _doc(std::move(doc)) {}

    const ShardingCoordinatorMetadata& getShardingCoordinatorMetadata() const override {
        return _doc.getShardingCoordinatorMetadata();
    }

    void setShardingCoordinatorMetadata(ShardingCoordinatorMetadata newMetadata) override {
        _doc.setShardingCoordinatorMetadata(std::move(newMetadata));
    }

    void setGenericPhase(CoordinatorGenericPhase p) override {
        if constexpr (kDocHasPhase) {
            _doc.setPhase(castToCoordinatorPhase(p));
        } else {
            tasserted(12096100, "Setting a phase to a non-recoverable document");
        }
    }

    CoordinatorGenericPhase getGenericPhase() const override {
        if constexpr (kDocHasPhase) {
            return castToGenericPhase(_doc.getPhase());
        } else {
            tasserted(12096101, "Getting the phase from a non-recoverable document");
        }
    }

    std::unique_ptr<CoordinatorStateDoc> clone() const override {
        return std::make_unique<CoordinatorStateDocImpl<StateDoc>>(_doc);
    }

    BSONObj toBSON() const override {
        return _doc.toBSON();
    }

    void replace(std::unique_ptr<CoordinatorStateDoc> newDoc) override {
        auto* cast = dynamic_cast<CoordinatorStateDocImpl<StateDoc>*>(newDoc.get());
        tassert(12096104, "newDoc dynamic_cast failed", cast);
        _doc = std::move(cast->_doc);
    }

    static constexpr CoordinatorGenericPhase castToGenericPhase(DocPhase en) {
        const auto i = static_cast<DocPhaseUnderlying>(en);
        return static_cast<CoordinatorGenericPhase>(i);
    }

    static constexpr DocPhase castToCoordinatorPhase(CoordinatorGenericPhase en) {
        const auto i = static_cast<DocPhaseUnderlying>(en);
        return static_cast<DocPhase>(i);
    }

    StateDoc _doc;
};

/**
 * Provide a typed StateDoc _doc for coordinator implementations.
 */
template <typename TStateDoc>
class MONGO_MOD_UNFORTUNATELY_OPEN NonRecoverableTypedDocMixin {
protected:
    using StateDoc = TStateDoc;
    using Phase = CoordinatorStateDocImpl<TStateDoc>::DocPhase;

    /**
     * Specify if a given phase is under holding the critical section.
     * These phases should finish as soon as possible in order to release the critical section, so
     * the opCtxs made under them automatically marked as non-deprioritizable.
     */
    virtual bool isInCriticalSection(Phase phase) const {
        return false;
    }

    explicit NonRecoverableTypedDocMixin(const BSONObj& coorDoc)
        : _docWrapper(
              StateDoc::parseOwned(coorDoc.getOwned(), IDLParserContext("CoordinatorDocument"))) {}

    CoordinatorStateDocImpl<TStateDoc> _docWrapper{};
    // Keep API compatibility for coordinators.
    StateDoc& _doc = _docWrapper._doc;
};


/**
 * Provide typed protected functions for recoverable coordinators.
 */
template <typename Base, typename TStateDoc>
class MONGO_MOD_UNFORTUNATELY_OPEN RecoverableTypedDocMixin
    : protected NonRecoverableTypedDocMixin<TStateDoc> {

protected:
    using StateDoc = NonRecoverableTypedDocMixin<TStateDoc>::StateDoc;
    using Phase = NonRecoverableTypedDocMixin<TStateDoc>::Phase;

    using NonRecoverableTypedDocMixin<TStateDoc>::NonRecoverableTypedDocMixin;

    std::function<void()> _buildPhaseHandler(Phase newPhase,
                                             std::function<void(OperationContext*)>&& handlerFn) {
        return self()._buildPhaseHandlerGeneric(
            CoordinatorStateDocImpl<TStateDoc>::castToGenericPhase(newPhase), std::move(handlerFn));
    }

    std::function<void()> _buildPhaseHandler(Phase newPhase,
                                             std::function<bool(OperationContext*)>&& shouldExecute,
                                             std::function<void(OperationContext*)>&& handlerFn) {
        return self()._buildPhaseHandlerGeneric(
            CoordinatorStateDocImpl<TStateDoc>::castToGenericPhase(newPhase),
            std::move(shouldExecute),
            std::move(handlerFn));
    }

    void _enterPhase(Phase newPhase) {
        self()._enterPhaseGeneric(CoordinatorStateDocImpl<TStateDoc>::castToGenericPhase(newPhase));
    }

    void _insertStateDocument(OperationContext* opCtx, StateDoc&& newDoc) {
        self()._insertStateDocumentGeneric(
            opCtx, std::make_unique<CoordinatorStateDocImpl<StateDoc>>(std::move(newDoc)));
    }

    void _updateStateDocument(OperationContext* opCtx, StateDoc&& newDoc) {
        self()._updateStateDocumentGeneric(
            opCtx, std::make_unique<CoordinatorStateDocImpl<StateDoc>>(std::move(newDoc)));
    }

    StringData serializePhase(Phase phase) const {
        return idl::serialize(phase);
    }

    StringData serializePhase(CoordinatorGenericPhase phase) const {
        return serializePhase(CoordinatorStateDocImpl<StateDoc>::castToCoordinatorPhase(phase));
    }

    StateDoc _copyDoc() const {
        stdx::lock_guard lk{self()._docMutex};
        return this->_doc;
    }

private:
    // CRTP self() functions.
    Base& self() {
        return static_cast<Base&>(*this);
    }
    const Base& self() const {
        return static_cast<Base const&>(*this);
    }
};

#undef MONGO_LOGV2_DEFAULT_COMPONENT

}  // namespace mongo
