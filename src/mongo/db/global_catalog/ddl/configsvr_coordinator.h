// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/primary_only_service_helpers/all_shards_and_config_causality_barrier.h"
#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

[[MONGO_MOD_NEEDS_REPLACEMENT]] ConfigsvrCoordinatorMetadata extractConfigsvrCoordinatorMetadata(
    const BSONObj& stateDoc);

/**
 * ConfigsvrCoordinators are POS instances that run on the configsvr and represent cluster
 * operations that are driven by the configsvr. ConfigsvrCoordinator implements common framework for
 * such operations. Concrete operations extend ConfigsvrCoordinator and implement their specific
 * bussiness logic on '_runImpl'
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ConfigsvrCoordinator
    : public repl::PrimaryOnlyService::TypedInstance<ConfigsvrCoordinator> {
public:
    explicit ConfigsvrCoordinator(const BSONObj& stateDoc);

    ~ConfigsvrCoordinator() override;

    SharedSemiFuture<void> getCompletionFuture() {
        return _completionPromise.getFuture();
    }

    virtual bool hasSameOptions(const BSONObj&) const = 0;

    ConfigsvrCoordinatorTypeEnum coordinatorType() const {
        return _coordId.getCoordinatorType();
    }

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override {}

protected:
    const ConfigsvrCoordinatorId _coordId;

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept final;

    virtual ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) noexcept = 0;

    /**
     * Invoked by `run()` at the start of every execution. Coordinators that persist a session
     * override this to perform a causality barrier that invalidates any retryable writes issued by
     * previous executions (an earlier attempt of this instance, or a previous primary) before doing
     * any work. The override is expected to be a no-op when no session has been persisted yet, so
     * it is safe to call unconditionally, including on the very first execution.
     */
    virtual void _performCausalityBarrier(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token) {}

    virtual const ConfigsvrCoordinatorMetadata& metadata() const = 0;

    void interrupt(Status status) noexcept final;

    void _removeStateDocument(OperationContext* opCtx);

    /**
     * Create an `OperationContext`. Provided for consistency with `ShardingCoordinator`,
     * which provides a similar method which also sets the `ForwardableOperationMetadata`.
     * Prefer this to `cc().makeOperationContext()`.
     */
    ServiceContext::UniqueOperationContext makeOperationContext() {
        return cc().makeOperationContext();
    }

    std::mutex _mutex;
    SharedPromise<void> _completionPromise;

private:
    virtual void _onCleanup(OperationContext* opCtx) {}
};

template <class StateDoc, class Phase>
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] ConfigsvrCoordinatorImpl
    : public ConfigsvrCoordinator,
      public OperationSessionPersistence {
public:
    ConfigsvrCoordinatorImpl(const BSONObj& stateDoc)
        : ConfigsvrCoordinator(stateDoc),
          _doc(StateDoc::parse(stateDoc, IDLParserContext("CoordinatorDocument"))),
          _sessionTracker(this) {}

    ~ConfigsvrCoordinatorImpl() override {}

protected:
    /**
     * Persists the given StateDoc in memory and sets `_doc` to the new value.
     * Note: We assume only one thread make writes on `_doc` (which is the executor thread) while
     * multiple threads can read it.
     */
    void _updateStateDocument(OperationContext* opCtx, StateDoc&& newDoc) {
        PersistentTaskStore<StateDoc> store(NamespaceString::kConfigsvrCoordinatorsNamespace);
        store.update(opCtx,
                     BSON(StateDoc::kIdFieldName << newDoc.getId().toBSON()),
                     newDoc.toBSON(),
                     defaultMajorityWriteConcern());

        {
            std::lock_guard lk{_docMutex};
            _doc = std::move(newDoc);
        }
    }

    /**
     * Updates _doc according to the given function and persists it in memory.
     * Note: We assume only one thread make writes on _doc (which is the executor thread) while
     * multiple threads can read it.
     */
    template <typename Func>
    void _updateStateDocumentWith(OperationContext* opCtx, Func&& updateF)
    requires(std::is_invocable_r_v<void, Func, StateDoc&>)
    {
        auto newDoc = _doc;

        updateF(newDoc);

        if (newDoc.getPhase() != Phase::kUnset) {
            _updateStateDocument(opCtx, std::move(newDoc));
        } else {
            std::lock_guard lk{_docMutex};
            _doc = std::move(newDoc);
        }
    }

    /**
     * Evaluates `_doc` under the `_docMutex` locking to protect the reads from concurrent writes.
     * Note: `_doc` needs to be accessed through this method when the calling thread is other than
     * the main coordinator thread. The reason is that writes are only done from the main
     * coordinator thread.
     */
    template <typename Func>
    auto _evalStateDocumentThreadSafe(Func&& evalF) const
    requires(std::is_invocable_v<Func, const StateDoc&>)
    {
        std::lock_guard lk{_docMutex};
        return evalF(_doc);
    }

    OperationSessionInfo _getNewSession(OperationContext* opCtx) {
        return _sessionTracker.getNextSession(opCtx);
    }

    void _releaseSession(OperationContext* opCtx) {
        _sessionTracker.releaseSession(opCtx);
    }

    boost::optional<OperationSessionInfo> readSession(OperationContext* opCtx) const override {
        const auto& session = metadata().getSession();
        if (!session) {
            return boost::none;
        }
        OperationSessionInfo osi;
        osi.setSessionId(session->getLsid());
        osi.setTxnNumber(session->getTxnNumber());
        return osi;
    }

    void writeSession(OperationContext* opCtx,
                      const boost::optional<OperationSessionInfo>& osi) override {
        if (!osi) {
            // The tracker will call writeSession with boost::none after calling releaseSession; by
            // the time the coordinator does this, we've already deleted our state document.
            return;
        }
        _updateStateDocumentWith(opCtx, [&](StateDoc& doc) {
            ConfigsvrCoordinatorMetadata newMetadata = doc.getConfigsvrCoordinatorMetadata();
            newMetadata.setSession(CoordinatorSession(*osi->getSessionId(), *osi->getTxnNumber()));
            doc.setConfigsvrCoordinatorMetadata(newMetadata);
        });
    }

    void _performCausalityBarrier(const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                  const CancellationToken& token) override {
        {
            // Only issue a barrier if a previous execution already established a session and may
            // therefore have issued retryable writes to participants. If no session has been
            // persisted yet there is nothing to invalidate.
            std::lock_guard lk{_docMutex};
            if (!_doc.getConfigsvrCoordinatorMetadata().getSession()) {
                return;
            }
        }

        // Bumps the session's txnNumber (persisting it with majority write concern) and performs a
        // noop retryable write on all shards and the config server. Any retryable write issued by a
        // previous execution (or by a rogue primary in a split-brain scenario) carries a lower
        // txnNumber on the same session and will therefore be rejected by the participants.
        auto opCtxHolder = makeOperationContext();
        auto* opCtx = opCtxHolder.get();
        auto barrier = _makeCausalityBarrier(executor, token);
        _sessionTracker.performCausalityBarrier(opCtx, *barrier);
    }

    /**
     * Builds the CausalityBarrier used by `_performCausalityBarrier`. Overridable so tests can
     * inject a barrier that records invocations instead of contacting participants.
     */
    virtual std::unique_ptr<CausalityBarrier> _makeCausalityBarrier(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token) {
        return std::make_unique<AllShardsAndConfigCausalityBarrier>(**executor, token);
    }

    template <typename Func>
    auto _buildPhaseHandler(const Phase& newPhase, Func&& handlerFn)
    requires(std::is_invocable_r_v<void, Func, OperationContext*>)
    {
        return [=, this] {
            const auto& currPhase = _doc.getPhase();

            if (currPhase > newPhase) {
                // Do not execute this phase if we already reached a subsequent one.
                return;
            }
            if (currPhase < newPhase) {
                // Persist the new phase if this is the first time we are executing it.
                _enterPhase(newPhase);
            }

            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            return handlerFn(opCtx);
        };
    }

    virtual std::string_view serializePhase(const Phase& phase) const = 0;

    void _enterPhase(Phase newPhase) {
        auto newDoc = _doc;

        newDoc.setPhase(newPhase);

        LOGV2_DEBUG(8355400,
                    2,
                    "ConfigsvrCoordinator phase transition",
                    "coordinatorId"_attr = _doc.getId(),
                    "newPhase"_attr = serializePhase(newDoc.getPhase()),
                    "oldPhase"_attr = serializePhase(_doc.getPhase()));

        auto opCtx = makeOperationContext();

        if (_doc.getPhase() == Phase::kUnset) {
            PersistentTaskStore<StateDoc> store(NamespaceString::kConfigsvrCoordinatorsNamespace);
            try {
                store.add(opCtx.get(), newDoc, defaultMajorityWriteConcern());
            } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
                // A series of step-up and step-down events can cause a node to try and insert the
                // document when it has already been persisted locally, but we must still wait for
                // majority commit.
                const auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
                const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
                WaitForMajorityService::get(opCtx->getServiceContext())
                    .waitUntilMajorityForWrite(lastLocalOpTime, opCtx.get()->getCancellationToken())
                    .get(opCtx.get());
            }
        } else {
            _updateStateDocument(opCtx.get(), std::move(newDoc));
        }
    }

    mutable std::mutex _docMutex;
    StateDoc _doc;

private:
    void _onCleanup(OperationContext* opCtx) override {
        _releaseSession(opCtx);
    }

    OperationSessionTracker _sessionTracker;
};

#undef MONGO_LOGV2_DEFAULT_COMPONENT

}  // namespace mongo
