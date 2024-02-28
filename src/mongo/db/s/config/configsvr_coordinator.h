/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <memory>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/configsvr_coordinator_gen.h"
#include "mongo/db/s/config/set_user_write_block_mode_coordinator_document_gen.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

ConfigsvrCoordinatorMetadata extractConfigsvrCoordinatorMetadata(const BSONObj& stateDoc);

/**
 * ConfigsvrCoordinators are POS instances that run on the configsvr and represent cluster
 * operations that are driven by the configsvr. ConfigsvrCoordinator implements common framework for
 * such operations. Concrete operations extend ConfigsvrCoordinator and implement their specific
 * bussiness logic on '_runImpl'
 */
class ConfigsvrCoordinator : public repl::PrimaryOnlyService::TypedInstance<ConfigsvrCoordinator> {
public:
    explicit ConfigsvrCoordinator(const BSONObj& stateDoc);

    ~ConfigsvrCoordinator();

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
                         const CancellationToken& token) noexcept override final;

    virtual ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) noexcept = 0;

    virtual const ConfigsvrCoordinatorMetadata& metadata() const = 0;

    void interrupt(Status status) noexcept override final;

    void _removeStateDocument(OperationContext* opCtx);

    OperationSessionInfo _getCurrentSession() const;

    Mutex _mutex = MONGO_MAKE_LATCH("ConfigsvrCoordinator::_mutex");
    SharedPromise<void> _completionPromise;
};

template <class StateDoc, class Phase>
class ConfigsvrCoordinatorImpl : public ConfigsvrCoordinator {
public:
    ConfigsvrCoordinatorImpl(const BSONObj& stateDoc)
        : ConfigsvrCoordinator(stateDoc),
          _doc(StateDoc::parse(IDLParserContext("CoordinatorDocument"), stateDoc)) {}

    ~ConfigsvrCoordinatorImpl() {}

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
                     WriteConcerns::kMajorityWriteConcernNoTimeout);

        {
            stdx::lock_guard lk{_docMutex};
            _doc = std::move(newDoc);
        }
    }

    /**
     * Updates _doc according to the given function and persists it in memory.
     * Note: We assume only one thread make writes on _doc (which is the executor thread) while
     * multiple threads can read it.
     */
    template <typename Func>
    void _updateStateDocumentWith(OperationContext* opCtx, Func&& updateF) requires(
        std::is_invocable_r_v<void, Func, StateDoc&>) {
        auto newDoc = _doc;

        updateF(newDoc);

        if (newDoc.getPhase() != Phase::kUnset) {
            _updateStateDocument(opCtx, std::move(newDoc));
        } else {
            stdx::lock_guard lk{_docMutex};
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
        requires(std::is_invocable_v<Func, const StateDoc&>) {
        stdx::lock_guard lk{_docMutex};
        return evalF(_doc);
    }

    /**
     * Gets a new session if necessary and updates `_doc`.
     */
    void _updateSession(OperationContext* opCtx) {
        auto internalSessionPool = InternalSessionPool::get(opCtx);

        _updateStateDocumentWith(opCtx, [&](StateDoc& doc) {
            ConfigsvrCoordinatorMetadata newMetadata = doc.getConfigsvrCoordinatorMetadata();

            const auto optPrevSession = doc.getSession();
            if (optPrevSession) {
                newMetadata.setSession(ConfigsvrCoordinatorSession(
                    optPrevSession->getLsid(), optPrevSession->getTxnNumber() + 1));
            } else {
                const auto newSession = internalSessionPool->acquireSystemSession();
                newMetadata.setSession(ConfigsvrCoordinatorSession(newSession.getSessionId(),
                                                                   newSession.getTxnNumber()));
            }

            doc.setConfigsvrCoordinatorMetadata(newMetadata);
        });
    }

    template <typename Func>
    auto _buildPhaseHandler(const Phase& newPhase,
                            Func&& handlerFn) requires(std::is_invocable_r_v<void, Func>) {
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
            return handlerFn();
        };
    }

    virtual StringData serializePhase(const Phase& phase) const = 0;

    void _enterPhase(Phase newPhase) {
        auto newDoc = _doc;

        newDoc.setPhase(newPhase);

        LOGV2_DEBUG(8355400,
                    2,
                    "ConfigsvrCoordinator phase transition",
                    "coordinatorId"_attr = _doc.getId(),
                    "newPhase"_attr = serializePhase(newDoc.getPhase()),
                    "oldPhase"_attr = serializePhase(_doc.getPhase()));

        auto opCtx = cc().makeOperationContext();

        if (_doc.getPhase() == Phase::kUnset) {
            PersistentTaskStore<StateDoc> store(NamespaceString::kConfigsvrCoordinatorsNamespace);
            try {
                store.add(opCtx.get(), newDoc, WriteConcerns::kMajorityWriteConcernNoTimeout);
            } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
                // A series of step-up and step-down events can cause a node to try and insert the
                // document when it has already been persisted locally, but we must still wait for
                // majority commit.
                const auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
                const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
                WaitForMajorityService::get(opCtx->getServiceContext())
                    .waitUntilMajorityForWrite(opCtx->getServiceContext(),
                                               lastLocalOpTime,
                                               opCtx.get()->getCancellationToken())
                    .get(opCtx.get());
            }
        } else {
            _updateStateDocument(opCtx.get(), std::move(newDoc));
        }
    }

    mutable Mutex _docMutex = MONGO_MAKE_LATCH("ConfigsvrCoordinatorImpl::_docMutex");
    StateDoc _doc;
};

#undef MONGO_LOGV2_DEFAULT_COMPONENT

}  // namespace mongo
