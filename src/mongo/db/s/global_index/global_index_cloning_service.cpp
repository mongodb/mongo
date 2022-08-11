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
      _execForCancelableOpCtx(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "GlobalIndexCloningServiceCancelableOpCtxPool";
          options.minThreads = 0;
          options.maxThreads = 1;
          return options;
      }())),
      _clonerState(std::move(clonerDoc)),
      _fetcherFactory(std::move(fetcherFactory)),
      _externalState(std::move(externalState)) {}

SemiFuture<void> GlobalIndexCloningService::CloningStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    auto abortToken = _initAbortSource(stepdownToken);
    _execForCancelableOpCtx->startup();
    _retryingCancelableOpCtxFactory.emplace(abortToken, _execForCancelableOpCtx);

    _init(executor);

    return ExecutorFuture(**executor)
        .then([this, executor, abortToken] { return _persistStateDocument(executor, abortToken); })
        .then([this, executor, abortToken] { return _runUntilDoneCloning(executor, abortToken); })
        // TODO: SERVER-68706 wait from coordinator to commit or abort.
        .onCompletion([this, stepdownToken](const Status& status) {
            _retryingCancelableOpCtxFactory.emplace(stepdownToken, _execForCancelableOpCtx);
            return status;
        })
        .then([this, executor, stepdownToken] { return _cleanup(executor, stepdownToken); })
        .thenRunOn(_cloningService->getInstanceCleanupExecutor())
        .onError([](const Status& status) {
            LOGV2(
                6755903, "Global index cloner encountered an error", "error"_attr = redact(status));
            return status;
        })
        .onCompletion([this, self = shared_from_this()](const Status& status) {
            if (!_completionPromise.getFuture().isReady()) {
                if (status.isOK()) {
                    _completionPromise.emplaceValue();
                } else {
                    _completionPromise.setError(status);
                }
            }
        })
        .semi();
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
                    _removeStateDocument(opCtx.get());
                });
        })
        .onTransientError([](const auto& status) {})
        .onUnrecoverableError([](const auto& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, stepdownToken);
}

void GlobalIndexCloningService::CloningStateMachine::_init(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    _inserter = std::make_unique<GlobalIndexInserter>(_clonerState.getNss(),
                                                      _clonerState.getIndexName(),
                                                      _clonerState.getIndexCollectionUUID(),
                                                      **executor);

    auto client = _serviceContext->makeClient("globalIndexClonerServiceInit");
    AlternativeClientRegion clientRegion(client);

    auto opCtx = _serviceContext->makeOperationContext(Client::getCurrent());

    auto routingInfo =
        _externalState->getShardedCollectionRoutingInfo(opCtx.get(), _clonerState.getNss());

    uassert(6755901,
            str::stream() << "Cannot create global index on unsharded ns "
                          << _clonerState.getNss().ns(),
            routingInfo.isSharded());

    auto myShardId = _externalState->myShardId(_serviceContext);

    auto indexKeyPattern =
        _clonerState.getIndexSpec().getObjectField(IndexDescriptor::kKeyPatternFieldName);
    _fetcher = _fetcherFactory->make(_clonerState.getNss(),
                                     _clonerState.getCollectionUUID(),
                                     _clonerState.getIndexCollectionUUID(),
                                     myShardId,
                                     _clonerState.getMinFetchTimestamp(),
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
                });
        })
        .onTransientError([](const Status& status) {

        })
        .onUnrecoverableError([](const Status& status) {

        })
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
            str::stream() << "new global index " << stateDoc << " is incompatible with ongoing "
                          << _clonerState.toBSON(),
            newCloning.getNss() == _clonerState.getNss() &&
                newCloning.getCollectionUUID() == _clonerState.getCollectionUUID());
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
    if (_clonerState.getState() > GlobalIndexClonerStateEnum::kUnused) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](auto& cancelableFactory) {
            auto opCtx = cancelableFactory.makeOperationContext(Client::getCurrent());

            GlobalIndexClonerDoc newDoc(_clonerState);
            newDoc.setState(GlobalIndexClonerStateEnum::kCloning);
            PersistentTaskStore<GlobalIndexClonerDoc> store(_cloningService->getStateDocumentsNS());
            store.add(opCtx.get(), newDoc, kNoWaitWriteConcern);

            std::swap(_clonerState, newDoc);

            LOGV2(6755904, "Persisted global index state document");
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, cancelToken);
}

void GlobalIndexCloningService::CloningStateMachine::_removeStateDocument(OperationContext* opCtx) {
    const auto& nss = _cloningService->getStateDocumentsNS();
    writeConflictRetry(
        opCtx, "GlobalIndexCloningStateMachine::removeStateDocument", nss.toString(), [&] {
            AutoGetCollection coll(opCtx, nss, MODE_IX);

            if (!coll) {
                return;
            }

            WriteUnitOfWork wuow(opCtx);

            // Set the promise when the delete commits, this is to ensure that any interruption that
            // happens later won't result in setting an error on the completion promise.
            opCtx->recoveryUnit()->onCommit([this](boost::optional<Timestamp> unusedCommitTime) {
                _completionPromise.emplaceValue();
            });

            deleteObjects(opCtx,
                          *coll,
                          nss,
                          BSON(GlobalIndexClonerDoc::kIndexCollectionUUIDFieldName
                               << _clonerState.getIndexCollectionUUID()),
                          true /* justOne */);

            wuow.commit();
        });
}

void GlobalIndexCloningService::CloningStateMachine::_initializeCollections(
    const CancelableOperationContextFactory& cancelableOpCtxFactory) {
    auto cancelableOpCtx = cancelableOpCtxFactory.makeOperationContext(Client::getCurrent());
    auto opCtx = cancelableOpCtx.get();

    resharding::data_copy::ensureCollectionExists(
        opCtx, skipIdNss(_clonerState.getNss(), _clonerState.getIndexName()), {});
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

}  // namespace global_index
}  // namespace mongo
