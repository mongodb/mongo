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
    const GlobalIndexCloningService* cloningService,
    std::unique_ptr<GlobalIndexCloningService::CloningExternalState> externalState,
    std::unique_ptr<GlobalIndexClonerFetcherFactoryInterface> fetcherFactory,
    GlobalIndexClonerDoc clonerDoc)
    : _serviceContext(serviceContext),
      _cloningService(cloningService),
      _indexCollectionUUID(clonerDoc.getIndexCollectionUUID()),
      _sourceNss(clonerDoc.getNss()),
      _sourceCollUUID(clonerDoc.getCollectionUUID()),
      _indexName(clonerDoc.getIndexName()),
      _indexSpec(clonerDoc.getIndexSpec().getOwned()),
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
      _externalState(std::move(externalState)) {}

SemiFuture<void> GlobalIndexCloningService::CloningStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    auto abortToken = _initAbortSource(stepdownToken);
    _execForCancelableOpCtx->startup();
    _retryingCancelableOpCtxFactory.emplace(abortToken, _execForCancelableOpCtx);

    _init(executor);

    _readyToCommitPromise.setFrom(ExecutorFuture(**executor)
                                      .then([this, executor, abortToken] {
                                          return _persistStateDocument(executor, abortToken);
                                      })
                                      .then([this, executor, abortToken] {
                                          return _runUntilDoneCloning(executor, abortToken);
                                      })
                                      .unsafeToInlineFuture());

    _completionPromise.setFrom(
        _readyToCommitPromise.getFuture()
            .thenRunOn(**executor)
            .then([this, stepdownToken] {
                _retryingCancelableOpCtxFactory.emplace(stepdownToken, _execForCancelableOpCtx);
            })
            .then([this, executor, stepdownToken] {
                return future_util::withCancellation(_waitForCleanupPromise.getFuture(),
                                                     stepdownToken);
            })
            .then([this, executor, stepdownToken] { return _cleanup(executor, stepdownToken); })
            .unsafeToInlineFuture()
            .tapError([](const Status& status) {
                LOGV2(6755903,
                      "Global index cloner encountered an error",
                      "error"_attr = redact(status));
            }));

    return _completionPromise.getFuture().semi();
}

void GlobalIndexCloningService::CloningStateMachine::interrupt(Status status) {}

void GlobalIndexCloningService::CloningStateMachine::abort() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_abortSource) {
        _abortSource->cancel();
    }
}

ExecutorFuture<void> GlobalIndexCloningService::CloningStateMachine::_cleanup(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor, stepdownToken](auto& cancelableFactory) {
            return ExecutorFuture(**executor)
                .then([this, executor, stepdownToken, &cancelableFactory] {
                    auto opCtx = cancelableFactory.makeOperationContext(&cc());
                    PersistentTaskStore<GlobalIndexClonerDoc> store(
                        _cloningService->getStateDocumentsNS());
                    store.remove(opCtx.get(),
                                 BSON(GlobalIndexClonerDoc::kIndexCollectionUUIDFieldName
                                      << _indexCollectionUUID));
                });
        })
        .onTransientError([](const auto& status) {})
        .onUnrecoverableError([](const auto& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, stepdownToken);
}

void GlobalIndexCloningService::CloningStateMachine::_init(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    _inserter = std::make_unique<GlobalIndexInserter>(
        _sourceNss, _indexName, _indexCollectionUUID, **executor);

    auto client = _serviceContext->makeClient("globalIndexClonerServiceInit");
    AlternativeClientRegion clientRegion(client);

    auto opCtx = _serviceContext->makeOperationContext(Client::getCurrent());

    auto routingInfo = _externalState->getShardedCollectionRoutingInfo(opCtx.get(), _sourceNss);

    uassert(6755901,
            str::stream() << "Cannot create global index on unsharded ns " << _sourceNss.ns(),
            routingInfo.isSharded());

    auto myShardId = _externalState->myShardId(_serviceContext);

    auto indexKeyPattern = _indexSpec.getObjectField(IndexDescriptor::kKeyPatternFieldName);
    _fetcher = _fetcherFactory->make(_sourceNss,
                                     _sourceCollUUID,
                                     _indexCollectionUUID,
                                     myShardId,
                                     _minFetchTimestamp,
                                     routingInfo.getShardKeyPattern().getKeyPattern(),
                                     indexKeyPattern.getOwned());
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
    // TODO: SERVER-68707
    return boost::none;
}

void GlobalIndexCloningService::CloningStateMachine::abort(bool isUserCancelled) {}

void GlobalIndexCloningService::CloningStateMachine::checkIfOptionsConflict(
    const BSONObj& stateDoc) const {
    auto newCloning =
        GlobalIndexClonerDoc::parse(IDLParserContext("globalIndexCloningCheckConflict"), stateDoc);

    uassert(6755900,
            str::stream() << "New global index " << stateDoc
                          << " is incompatible with ongoing global index build in namespace: "
                          << _sourceNss << ", uuid: " << _sourceCollUUID,
            newCloning.getNss() == _sourceNss && newCloning.getCollectionUUID() == _sourceCollUUID);
}

CancellationToken GlobalIndexCloningService::CloningStateMachine::_initAbortSource(
    const CancellationToken& stepdownToken) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _abortSource = CancellationSource(stepdownToken);
    }

    // TODO: SERVER-67563 Handle possible race between _initAbortSource and abort

    return _abortSource->token();
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

    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](auto& cancelableFactory) {
            auto opCtx = cancelableFactory.makeOperationContext(Client::getCurrent());

            PersistentTaskStore<GlobalIndexClonerDoc> store(_cloningService->getStateDocumentsNS());

            auto mutableState = _getMutableState();
            mutableState.setState(GlobalIndexClonerStateEnum::kReadyToCommit);

            BSONObj update(BSON("$set" << BSON(GlobalIndexClonerDoc::kMutableStateFieldName
                                               << mutableState.toBSON())));
            store.update(
                opCtx.get(),
                BSON(GlobalIndexClonerDoc::kIndexCollectionUUIDFieldName << _indexCollectionUUID),
                update);

            {
                stdx::unique_lock lk(_mutex);
                _mutableState = mutableState;
            }

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

    resharding::data_copy::ensureCollectionExists(opCtx, skipIdNss(_sourceNss, _indexName), {});
}

ExecutorFuture<void> GlobalIndexCloningService::CloningStateMachine::_clone(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& cancelToken,
    const CancelableOperationContextFactory& cancelableOpCtxFactory) {
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

               const auto& next = _fetchedDocs.front();

               auto cancelableOpCtx =
                   cancelableOpCtxFactory.makeOperationContext(Client::getCurrent());
               _inserter->processDoc(cancelableOpCtx.get(), next.indexKeyValues, next.documentKey);

               _fetchedDocs.pop();
           })
        .until([this](const Status& status) { return !status.isOK() || _fetchedDocs.empty(); })
        .on(**executor, cancelToken);
}

void GlobalIndexCloningService::CloningStateMachine::_ensureCollection(OperationContext* opCtx,
                                                                       const NamespaceString& nss) {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Create the destination collection if necessary.
    writeConflictRetry(opCtx, "CloningStateMachine::_ensureCollection", nss.toString(), [&] {
        const CollectionPtr coll =
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
    stdx::unique_lock lk(_mutex);

    if (!_waitForCleanupPromise.getFuture().isReady()) {
        _waitForCleanupPromise.emplaceValue();
    }

    // TODO: SERVER-67563 Implement abort
}

GlobalIndexClonerStateEnum GlobalIndexCloningService::CloningStateMachine::_getState() const {
    stdx::unique_lock lk(_mutex);
    return _mutableState.getState();
}

GlobalIndexClonerMutableState GlobalIndexCloningService::CloningStateMachine::_getMutableState()
    const {
    stdx::unique_lock lk(_mutex);
    return _mutableState;
}

GlobalIndexClonerDoc GlobalIndexCloningService::CloningStateMachine::_makeClonerDoc() const {
    GlobalIndexClonerDoc clonerDoc;
    clonerDoc.setIndexCollectionUUID(_indexCollectionUUID);
    clonerDoc.setNss(_sourceNss);
    clonerDoc.setCollectionUUID(_sourceCollUUID);
    clonerDoc.setIndexName(_indexName);
    clonerDoc.setIndexSpec(_indexSpec);
    clonerDoc.setMinFetchTimestamp(_minFetchTimestamp);

    {
        stdx::unique_lock lk(_mutex);
        clonerDoc.setMutableState(_mutableState);
    }

    return clonerDoc;
}

}  // namespace global_index
}  // namespace mongo
