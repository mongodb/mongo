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

#include <queue>

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/global_index/global_index_cloner_fetcher_factory.h"
#include "mongo/db/s/global_index/global_index_cloner_gen.h"
#include "mongo/db/s/global_index/global_index_inserter.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace global_index {

class GlobalIndexCloningService : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "GlobalIndexCloningService"_sd;

    explicit GlobalIndexCloningService(ServiceContext* serviceContext);

    class CloningExternalState;
    class CloningStateMachine;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kGlobalIndexClonerNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override;

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) override;

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override;

private:
    ServiceContext* const _serviceContext;
};

/**
 * Represents the current state of a global index operation on this shard. This class drives state
 * transitions and updates to underlying on-disk metadata.
 */
class GlobalIndexCloningService::CloningStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<CloningStateMachine> {
public:
    CloningStateMachine(
        ServiceContext* service,
        const GlobalIndexCloningService* cloningService,
        std::unique_ptr<GlobalIndexCloningService::CloningExternalState> externalState,
        std::unique_ptr<GlobalIndexClonerFetcherFactoryInterface> fetcherFactory,
        GlobalIndexClonerDoc clonerDoc);

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    void interrupt(Status status) override;

    void abort();

    SharedSemiFuture<void> getReadyToCommitFuture() const {
        return _readyToCommitPromise.getFuture();
    }

    /**
     * Returns a Future that will be resolved when all work associated with this Instance is done
     * making forward progress.
     */
    SharedSemiFuture<void> getCompletionFuture() const {
        return _completionPromise.getFuture();
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode,
        MongoProcessInterface::CurrentOpSessionsMode) noexcept override;

    /**
     * Initiates the cancellation of the cloning operation.
     */
    void abort(bool isUserCancelled);

    /**
     * Tells this cloner to perform cleanup. This can cause this cloner to abort if it is still
     * running.
     */
    void cleanup();

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final;

private:
    /**
     * Initializes the _abortSource and generates a token from it to return back the caller.
     * Should only be called once per lifetime.
     */
    CancellationToken _initAbortSource(const CancellationToken& stepdownToken);

    /**
     * Initializes the necessary components.
     */
    void _init(const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Make sure that the necessary collections are created.
     */
    void _initializeCollections(const CancelableOperationContextFactory& cancelableOpCtxFactory);

    /**
     * Inserts the state document to storage.
     */
    ExecutorFuture<void> _persistStateDocument(
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& cancelToken);

    ExecutorFuture<repl::OpTime> _transitionToReadyToCommit(
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& cancelToken);

    /**
     * Performs the entire cloning process.
     */
    ExecutorFuture<void> _runUntilDoneCloning(
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& stepdownToken);

    /**
     * Iterates over the documents from the source collection and convert them into global index
     * entries.
     */
    ExecutorFuture<void> _clone(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                const CancellationToken& cancelToken,
                                const CancelableOperationContextFactory& cancelableOpCtxFactory);

    /**
     * Removes the side collections created by this cloner.
     */
    ExecutorFuture<void> _cleanup(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& stepdownToken);

    /**
     * Fetches documents from source collection and store them in a queue.
     */
    void _fetchNextBatch(OperationContext* opCtx);

    /**
     * Convert fetched documents to global index entries.
     */
    ExecutorFuture<void> _processBatch(
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& cancelToken,
        const CancelableOperationContextFactory& cancelableOpCtxFactory);

    /**
     * Create collection with the given namespace only if it doesn't exist.
     */
    void _ensureCollection(OperationContext* opCtx, const NamespaceString& nss);

    GlobalIndexClonerStateEnum _getState() const;
    GlobalIndexClonerMutableState _getMutableState() const;
    GlobalIndexClonerDoc _makeClonerDoc() const;

    /**********************************************************************************
     * Thread safety legend
     *
     * (TS) - Thread safe. Object can be accessed concurrently without additional mutex.
     * (NC) - No concurrent access pattern. So can be used without mutex.
     * (M) - Mutex required.
     */

    ServiceContext* const _serviceContext;  // (TS)

    // The primary-only service instance corresponding to the cloner instance. Not owned.
    const GlobalIndexCloningService* const _cloningService;  // (TS)

    const UUID _indexCollectionUUID;
    const NamespaceString _sourceNss;
    const UUID _sourceCollUUID;
    const std::string _indexName;
    const BSONObj _indexSpec;
    const Timestamp _minFetchTimestamp;

    // A separate executor different from the one supplied by the primary only service is needed
    // because the one from POS can be shut down during step down. This will ensure that the
    // operation context created from the cancelableOpCtxFactory can be interrupted when the cancel
    // token is aborted during step down.
    const std::shared_ptr<ThreadPool> _execForCancelableOpCtx;  // (TS)

    boost::optional<resharding::RetryingCancelableOperationContextFactory>
        _retryingCancelableOpCtxFactory;  // (TS)

    mutable Mutex _mutex = MONGO_MAKE_LATCH("GlobalIndexCloningStateMachine::_mutex");

    GlobalIndexClonerMutableState _mutableState;  // (NC)

    // Canceled when there is an unrecoverable error or stepdown.
    boost::optional<CancellationSource> _abortSource;  // (M)

    std::unique_ptr<GlobalIndexClonerFetcherFactoryInterface> _fetcherFactory;  // (TS)
    std::unique_ptr<GlobalIndexClonerFetcherInterface> _fetcher;                // (NC)
    std::unique_ptr<GlobalIndexInserter> _inserter;                             // (NC)

    // Keeps track if there is still a posibility that we still have documents that needs to be
    // fetched from the source collection.
    bool _hasMoreToFetch{true};  // (NC)

    std::queue<GlobalIndexClonerFetcher::FetchedEntry> _fetchedDocs;  // (NC)

    SharedPromise<void> _completionPromise;                      // (TS)
    SharedPromise<void> _readyToCommitPromise;                   // (TS)
    SharedPromise<void> _waitForCleanupPromise;                  // (M)
    const std::unique_ptr<CloningExternalState> _externalState;  // (TS)
};

}  // namespace global_index
}  // namespace mongo
