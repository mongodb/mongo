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

#include "mongo/db/s/global_index/global_index_cloning_service.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/global_index/global_index_cloning_external_state.h"
#include "mongo/db/s/global_index/global_index_server_parameters_gen.h"
#include "mongo/db/s/global_index/global_index_util.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kGlobalIndex

namespace mongo {
namespace global_index {

namespace {

const WriteConcernOptions kNoWaitWriteConcern{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

}  // namespace

GlobalIndexCloningService::GlobalIndexCloningService(ServiceContext* serviceContext)
    : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}

ThreadPool::Limits GlobalIndexCloningService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimit;
    threadPoolLimit.maxThreads = gGlobalIndexClonerServiceMaxThreadCount;
    return threadPoolLimit;
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> GlobalIndexCloningService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<GlobalIndexCloningService::CloningStateMachine>(
        _serviceContext,
        this,
        std::make_unique<GlobalIndexCloningStateImpl>(),
        std::make_unique<GlobalIndexClonerFetcherFactory>(),
        GlobalIndexClonerDoc::parse(IDLParserContext{"GlobalIndexCloner"}, initialState));
}

void GlobalIndexCloningService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialStateDoc,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    // There are no restrictions on running concurrent global index instances.
}

GlobalIndexCloningService::CloningStateMachine::CloningStateMachine(
    ServiceContext* serviceContext,
    GlobalIndexCloningService* cloningService,
    std::unique_ptr<GlobalIndexCloningService::CloningExternalState> externalState,
    std::unique_ptr<GlobalIndexClonerFetcherFactoryInterface> fetcherFactory,
    GlobalIndexClonerDoc clonerDoc)
    : _serviceContext(serviceContext),
      _cloningService(cloningService),
      _metadata(clonerDoc.getCommonGlobalIndexMetadata()),
      _minFetchTimestamp(clonerDoc.getMinFetchTimestamp()),
      _execForCancelableOpCtx(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "GlobalIndexCloningServiceCancelableOpCtxPool";
          options.minThreads = 0;
          options.maxThreads = 1;
          return options;
      }())),
      _mutableState(clonerDoc.getMutableState()),
      _fetcherFactory(std::move(fetcherFactory)),
      _externalState(std::move(externalState)),
      _metrics{GlobalIndexMetrics::initializeFrom(clonerDoc, serviceContext)} {}

SemiFuture<void> GlobalIndexCloningService::CloningStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    auto cleanupToken = _initCleanupToken(stepdownToken);
    _execForCancelableOpCtx->startup();
    _retryingCancelableOpCtxFactory.emplace(cleanupToken, _execForCancelableOpCtx);

    _init(executor);

    _readyToCommitPromise.setFrom(ExecutorFuture(**executor)
                                      .then([this, executor, cleanupToken] {
                                          return _persistStateDocument(executor, cleanupToken);
                                      })
                                      .then([this, executor, cleanupToken] {
                                          return _runUntilDoneCloning(executor, cleanupToken);
                                      })
                                      .unsafeToInlineFuture());

    _completionPromise.setFrom(
        _readyToCommitPromise.getFuture()
            .thenRunOn(**executor)
            .then([this, executor, stepdownToken] {
                return future_util::withCancellation(_waitForCleanupPromise.getFuture(),
                                                     stepdownToken);
            })
            .thenRunOn(_cloningService->getInstanceCleanupExecutor())
            // The shared_ptr stored in the PrimaryOnlyService's map for the this instance can be
            // cleared during replication step up. Stashing a shared_ptr to the callback will
            // ensure that this instance will be kept alive while the callback is being used.
            .onCompletion([this, cleanupToken, stepdownToken, self = shared_from_this()](
                              const Status& status) {
                _retryingCancelableOpCtxFactory.emplace(stepdownToken, _execForCancelableOpCtx);

                auto wasCleanupTriggered = cleanupToken.isCanceled() && !stepdownToken.isCanceled();
                if (status.isOK() || wasCleanupTriggered) {
                    return _cleanup(_cloningService->getInstanceCleanupExecutor(), stepdownToken)
                        .semi();
                }

                return SemiFuture<void>(status);
            })
            .unsafeToInlineFuture()
            .tapError([](const Status& status) {
                LOGV2(6755903,
                      "Global index cloner encountered an error",
                      "error"_attr = redact(status));
            }));

    return _completionPromise.getFuture().semi();
}

void GlobalIndexCloningService::CloningStateMachine::interrupt(Status status) {
    // This service relies on the cancellation of the token from the primary only service and
    // ignores the interrupts.
}

ExecutorFuture<void> GlobalIndexCloningService::CloningStateMachine::_cleanup(
    std::shared_ptr<executor::TaskExecutor> executor, const CancellationToken& stepdownToken) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor, stepdownToken](auto& cancelableFactory) {
            return ExecutorFuture(executor).then(
                [this, executor, stepdownToken, &cancelableFactory] {
                    auto opCtx = cancelableFactory.makeOperationContext(&cc());

                    const BSONObj instanceId(
                        BSON(GlobalIndexClonerDoc::kIndexCollectionUUIDFieldName
                             << _metadata.getIndexCollectionUUID()));

                    DBDirectClient client(opCtx.get());
                    auto result = client.remove([&] {
                        write_ops::DeleteCommandRequest deleteRequest(
                            _cloningService->getStateDocumentsNS());

                        write_ops::DeleteOpEntry deleteStateDoc;
                        deleteStateDoc.setQ(instanceId);
                        deleteStateDoc.setMulti(false);
                        deleteRequest.setDeletes({deleteStateDoc});

                        return deleteRequest;
                    }());

                    if (const auto& errorList = result.getWriteErrors()) {
                        uassertStatusOK(errorList->front().getStatus());
                    }

                    if (result.getN() != 1) {
                        // This is to handle cases where the state document hasn't been inserted.
                        _cloningService->releaseInstance(instanceId, Status::OK());
                    }

                    _metrics->onStateTransition(GlobalIndexClonerStateEnum::kDone, boost::none);
                });
        })
        .onTransientError([](const auto& status) {})
        .onUnrecoverableError([](const auto& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(executor, stepdownToken);
}

void GlobalIndexCloningService::CloningStateMachine::_init(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    const auto& indexSpec = _metadata.getIndexSpec();
    _inserter = std::make_unique<GlobalIndexInserter>(
        _metadata.getNss(), indexSpec.getName(), _metadata.getIndexCollectionUUID(), **executor);

    auto client = _serviceContext->makeClient("globalIndexClonerServiceInit");
    AlternativeClientRegion clientRegion(client);

    auto opCtx = _serviceContext->makeOperationContext(Client::getCurrent());

    auto routingInfo =
        _externalState->getShardedCollectionPlacementInfo(opCtx.get(), _metadata.getNss());

    uassert(6755901,
            str::stream() << "Cannot create global index on unsharded ns "
                          << _metadata.getNss().toStringForErrorMsg(),
            routingInfo.isSharded());

    auto myShardId = _externalState->myShardId(_serviceContext);

    _fetcher = _fetcherFactory->make(_metadata.getNss(),
                                     _metadata.getCollectionUUID(),
                                     _metadata.getIndexCollectionUUID(),
                                     myShardId,
                                     _minFetchTimestamp,
                                     routingInfo.getShardKeyPattern().getKeyPattern(),
                                     indexSpec.getKey().getOwned());
    if (auto id = _mutableState.getLastProcessedId()) {
        _fetcher->setResumeId(*id);
    }
}

ExecutorFuture<void> GlobalIndexCloningService::CloningStateMachine::_runUntilDoneCloning(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& cancelToken) {

    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor, cancelToken](auto& cancelableFactory) {
            return ExecutorFuture(**executor)
                .then([this, executor, &cancelableFactory] {
                    return _initializeCollections(cancelableFactory);
                })
                .then([this, executor, cancelToken, cancelableFactory] {
                    return _clone(executor, cancelToken, cancelableFactory);
                })
                .then([this, executor, cancelToken, cancelableFactory] {
                    return _transitionToReadyToCommit(executor, cancelToken);
                })
                .then([this, cancelToken](const repl::OpTime& readyToCommitOpTime) {
                    return WaitForMajorityService::get(_serviceContext)
                        .waitUntilMajority(readyToCommitOpTime, cancelToken);
                });
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, cancelToken);
}

boost::optional<BSONObj> GlobalIndexCloningService::CloningStateMachine::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode,
    MongoProcessInterface::CurrentOpSessionsMode) noexcept {
    return _metrics->reportForCurrentOp();
}

void GlobalIndexCloningService::CloningStateMachine::checkIfOptionsConflict(
    const BSONObj& stateDoc) const {
    auto newCloning =
        GlobalIndexClonerDoc::parse(IDLParserContext("globalIndexCloningCheckConflict"), stateDoc);

    uassert(6755900,
            str::stream() << "New global index " << stateDoc
                          << " is incompatible with ongoing global index build in namespace: "
                          << _metadata.getNss().toStringForErrorMsg()
                          << ", uuid: " << _metadata.getCollectionUUID(),
            newCloning.getNss() == _metadata.getNss() &&
                newCloning.getCollectionUUID() == _metadata.getCollectionUUID());
}

CancellationToken GlobalIndexCloningService::CloningStateMachine::_initCleanupToken(
    const CancellationToken& stepdownToken) {
    auto cleanupCalled = ([&] {
        stdx::lock_guard<Latch> lk(_mutex);
        _cleanupSignalSource = CancellationSource(stepdownToken);

        return _cleanupCalled;
    })();

    if (cleanupCalled) {
        // cleanup request came in before _cleanupSignalSource was initialized,
        // so we should cancel it.
        _cleanupSignalSource->cancel();
    }

    return _cleanupSignalSource->token();
}

ExecutorFuture<void> GlobalIndexCloningService::CloningStateMachine::_persistStateDocument(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& cancelToken) {
    if (_getState() > GlobalIndexClonerStateEnum::kUnused) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](auto& cancelableFactory) {
            auto opCtx = cancelableFactory.makeOperationContext(Client::getCurrent());

            auto newDoc = _makeClonerDoc();
            newDoc.getMutableState().setState(GlobalIndexClonerStateEnum::kCloning);
            PersistentTaskStore<GlobalIndexClonerDoc> store(_cloningService->getStateDocumentsNS());
            store.add(opCtx.get(), newDoc, kNoWaitWriteConcern);

            LOGV2(6755904, "Persisted global index state document");

            {
                stdx::unique_lock lk(_mutex);
                _mutableState.setState(GlobalIndexClonerStateEnum::kCloning);
            }

            _metrics->onStateTransition(boost::none, GlobalIndexClonerStateEnum::kCloning);
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, cancelToken);
}

ExecutorFuture<repl::OpTime>
GlobalIndexCloningService::CloningStateMachine::_transitionToReadyToCommit(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& cancelToken) {
    if (_getState() > GlobalIndexClonerStateEnum::kReadyToCommit) {
        // If we recovered from disk, then primary only service would have already waited for
        // majority, so just return an empty opTime.
        return ExecutorFuture<repl::OpTime>(**executor, repl::OpTime());
    }

    _metrics->setEndFor(GlobalIndexMetrics::TimedPhase::kCloning, now());

    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](auto& cancelableFactory) {
            auto opCtx = cancelableFactory.makeOperationContext(Client::getCurrent());

            auto mutableState = _mutableState;
            mutableState.setState(GlobalIndexClonerStateEnum::kReadyToCommit);
            _updateMutableState(opCtx.get(), std::move(mutableState));

            return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {})
        .until<StatusWith<repl::OpTime>>(
            [](const StatusWith<repl::OpTime>& status) { return status.isOK(); })
        .on(**executor, cancelToken);
}

void GlobalIndexCloningService::CloningStateMachine::_initializeCollections(
    const CancelableOperationContextFactory& cancelableOpCtxFactory) {
    auto cancelableOpCtx = cancelableOpCtxFactory.makeOperationContext(Client::getCurrent());
    auto opCtx = cancelableOpCtx.get();

    resharding::data_copy::ensureCollectionExists(
        opCtx, skipIdNss(_metadata.getNss(), _metadata.getIndexSpec().getName()), {});
}

ExecutorFuture<void> GlobalIndexCloningService::CloningStateMachine::_clone(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& cancelToken,
    const CancelableOperationContextFactory& cancelableOpCtxFactory) {
    if (_getState() > GlobalIndexClonerStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor);
    }

    _metrics->setStartFor(GlobalIndexMetrics::TimedPhase::kCloning, now());

    return AsyncTry([this, executor, cancelToken, cancelableOpCtxFactory] {
               auto cancelableOpCtx =
                   cancelableOpCtxFactory.makeOperationContext(Client::getCurrent());
               _fetchNextBatch(cancelableOpCtx.get());

               return _processBatch(executor, cancelToken, cancelableOpCtxFactory);
           })
        .until([this](const Status& status) { return !status.isOK() || !_hasMoreToFetch; })
        .on(**executor, cancelToken);
}

void GlobalIndexCloningService::CloningStateMachine::_fetchNextBatch(OperationContext* opCtx) {
    if (!_fetchedDocs.empty()) {
        // There are still documents that haven't not been processed from the previous attempt.
        return;
    }

    int totalSize = 0;
    Timer timer;
    ON_BLOCK_EXIT([&] {
        _metrics->onCloningRemoteBatchRetrieval(duration_cast<Milliseconds>(timer.elapsed()));
    });

    do {
        if (auto next = _fetcher->getNext(opCtx)) {
            totalSize += next->indexKeyValues.objsize();
            _fetchedDocs.push(*next);
        } else {
            _hasMoreToFetch = false;
        }
    } while (totalSize < gGlobalIndexClonerServiceFetchBatchMaxSizeBytes && _hasMoreToFetch);
}

ExecutorFuture<void> GlobalIndexCloningService::CloningStateMachine::_processBatch(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& cancelToken,
    const CancelableOperationContextFactory& cancelableOpCtxFactory) {
    return AsyncTry([this, &cancelableOpCtxFactory] {
               if (_fetchedDocs.empty()) {
                   return;
               }

               Timer timer;
               const auto& next = _fetchedDocs.front();

               auto cancelableOpCtx =
                   cancelableOpCtxFactory.makeOperationContext(Client::getCurrent());
               _inserter->processDoc(cancelableOpCtx.get(), next.indexKeyValues, next.documentKey);

               _lastProcessedIdSinceStepUp = Value(next.documentKey["_id"]);
               _fetcher->setResumeId(_lastProcessedIdSinceStepUp);

               _fetchedDocs.pop();

               _metrics->onDocumentsProcessed(1,
                                              next.documentKey.objsize() +
                                                  next.indexKeyValues.objsize(),
                                              duration_cast<Milliseconds>(timer.elapsed()));
           })
        .until([this](const Status& status) { return !status.isOK() || _fetchedDocs.empty(); })
        .on(**executor, cancelToken)
        .then([this, executor, cancelToken] {
            if (_lastProcessedIdSinceStepUp.missing()) {
                return ExecutorFuture<void>(**executor);
            }

            return _retryingCancelableOpCtxFactory
                ->withAutomaticRetry([this, executor](auto& cancelableFactory) {
                    auto opCtx = cancelableFactory.makeOperationContext(Client::getCurrent());

                    auto mutableState = _mutableState;
                    mutableState.setLastProcessedId(_lastProcessedIdSinceStepUp);
                    _updateMutableState(opCtx.get(), std::move(mutableState));
                })
                .onTransientError([](const Status& status) {})
                .onUnrecoverableError([](const Status& status) {})
                .until<Status>([](const Status& status) { return status.isOK(); })
                .on(**executor, cancelToken);
        });
}

void GlobalIndexCloningService::CloningStateMachine::_ensureCollection(OperationContext* opCtx,
                                                                       const NamespaceString& nss) {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Create the destination collection if necessary.
    writeConflictRetry(opCtx, "CloningStateMachine::_ensureCollection", nss.toString(), [&] {
        const Collection* coll =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        if (coll) {
            return;
        }

        WriteUnitOfWork wuow(opCtx);
        AutoGetDb autoDb(opCtx, nss.dbName(), LockMode::MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto db = autoDb.ensureDbExists(opCtx);

        CollectionOptions options;
        db->createCollection(opCtx, nss, options);
        wuow.commit();
    });
}

void GlobalIndexCloningService::CloningStateMachine::cleanup() {
    auto cleanupSource = ([&] {
        stdx::lock_guard lk(_mutex);

        if (!_waitForCleanupPromise.getFuture().isReady()) {
            _waitForCleanupPromise.emplaceValue();
        }

        _cleanupCalled = true;

        return _cleanupSignalSource;
    })();

    if (cleanupSource) {
        cleanupSource->cancel();
    }
}

GlobalIndexClonerStateEnum GlobalIndexCloningService::CloningStateMachine::_getState() const {
    return _mutableState.getState();
}

GlobalIndexClonerDoc GlobalIndexCloningService::CloningStateMachine::_makeClonerDoc() const {
    GlobalIndexClonerDoc clonerDoc;
    clonerDoc.setCommonGlobalIndexMetadata(_metadata);
    clonerDoc.setMinFetchTimestamp(_minFetchTimestamp);

    {
        stdx::unique_lock lk(_mutex);
        clonerDoc.setMutableState(_mutableState);
    }

    return clonerDoc;
}

void GlobalIndexCloningService::CloningStateMachine::_updateMutableState(
    OperationContext* opCtx, GlobalIndexClonerMutableState newMutableState) {
    PersistentTaskStore<GlobalIndexClonerDoc> store(_cloningService->getStateDocumentsNS());
    BSONObj update(BSON(
        "$set" << BSON(GlobalIndexClonerDoc::kMutableStateFieldName << newMutableState.toBSON())));
    store.update(opCtx,
                 BSON(GlobalIndexClonerDoc::kIndexCollectionUUIDFieldName
                      << _metadata.getIndexCollectionUUID()),
                 update);

    const auto oldState = _mutableState.getState();
    _mutableState = std::move(newMutableState);
    _metrics->onStateTransition(oldState, _mutableState.getState());
}

Date_t GlobalIndexCloningService::CloningStateMachine::now() const {
    return _serviceContext->getFastClockSource()->now();
}

}  // namespace global_index
}  // namespace mongo
