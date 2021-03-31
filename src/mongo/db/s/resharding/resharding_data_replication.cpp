/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_data_replication.h"

#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_txn_cloner.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future_util.h"

namespace mongo {
namespace {

/**
 * Fulfills the promise if it isn't already fulfilled. Does nothing otherwise.
 */
void ensureFulfilledPromise(SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

/**
 * Fulfills the promise with an error if it isn't already fulfilled. Does nothing otherwise.
 */
void ensureFulfilledPromise(SharedPromise<void>& sp, Status error) {
    if (!sp.getFuture().isReady()) {
        sp.setError(error);
    }
}

/**
 * Converts a vector of SharedSemiFutures into a vector of ExecutorFutures.
 */
std::vector<ExecutorFuture<void>> thenRunAllOn(
    const std::vector<SharedSemiFuture<void>>& futures,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    std::vector<ExecutorFuture<void>> result;
    result.reserve(futures.size());

    for (const auto& future : futures) {
        result.emplace_back(future.thenRunOn(executor));
    }

    return result;
}

/**
 * Given a vector of input futures, returns a future that becomes ready when either
 *
 *  (a) all of the input futures have become ready with success, or
 *  (b) one of the input futures has become ready with an error.
 *
 * This function returns an immediately ready future when the vector of input futures is empty.
 */
ExecutorFuture<void> whenAllSucceedOn(const std::vector<SharedSemiFuture<void>>& futures,
                                      std::shared_ptr<executor::TaskExecutor> executor) {
    return !futures.empty()
        ? whenAllSucceed(thenRunAllOn(futures, executor)).thenRunOn(std::move(executor))
        : ExecutorFuture(std::move(executor));
}

/**
 * Given a vector of input futures, returns a future that becomes ready when all of the input
 * futures have become ready with success or failure.
 *
 * This function returns an immediately ready future when the vector of input futures is empty.
 */
ExecutorFuture<void> whenAllSettledOn(const std::vector<SharedSemiFuture<void>>& futures,
                                      std::shared_ptr<executor::TaskExecutor> executor) {
    return !futures.empty()
        ? whenAll(thenRunAllOn(futures, executor)).ignoreValue().thenRunOn(std::move(executor))
        : ExecutorFuture(std::move(executor));
}

}  // namespace

std::unique_ptr<ReshardingCollectionCloner> ReshardingDataReplication::_makeCollectionCloner(
    ReshardingMetrics* metrics,
    const CommonReshardingMetadata& metadata,
    const ShardId& myShardId,
    Timestamp fetchTimestamp) {
    return std::make_unique<ReshardingCollectionCloner>(
        std::make_unique<ReshardingCollectionCloner::Env>(metrics),
        ShardKeyPattern{metadata.getReshardingKey()},
        metadata.getSourceNss(),
        metadata.getSourceUUID(),
        myShardId,
        fetchTimestamp,
        metadata.getTempReshardingNss());
}

std::vector<std::unique_ptr<ReshardingTxnCloner>> ReshardingDataReplication::_makeTxnCloners(
    const CommonReshardingMetadata& metadata,
    const std::vector<ShardId>& donorShardIds,
    Timestamp fetchTimestamp) {
    std::vector<std::unique_ptr<ReshardingTxnCloner>> txnCloners;
    txnCloners.reserve(donorShardIds.size());

    for (const auto& donor : donorShardIds) {
        txnCloners.emplace_back(std::make_unique<ReshardingTxnCloner>(
            ReshardingSourceId(metadata.getReshardingUUID(), donor), fetchTimestamp));
    }

    return txnCloners;
}

std::vector<std::unique_ptr<ReshardingOplogFetcher>> ReshardingDataReplication::_makeOplogFetchers(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const CommonReshardingMetadata& metadata,
    const std::vector<ShardId>& donorShardIds,
    Timestamp fetchTimestamp,
    const ShardId& myShardId) {
    std::vector<std::unique_ptr<ReshardingOplogFetcher>> oplogFetchers;
    oplogFetchers.reserve(donorShardIds.size());

    for (const auto& donor : donorShardIds) {
        auto oplogBufferNss = getLocalOplogBufferNamespace(metadata.getSourceUUID(), donor);
        auto idToResumeFrom =
            resharding::getFetcherIdToResumeFrom(opCtx, oplogBufferNss, fetchTimestamp);
        invariant((idToResumeFrom >= ReshardingDonorOplogId{fetchTimestamp, fetchTimestamp}));

        oplogFetchers.emplace_back(std::make_unique<ReshardingOplogFetcher>(
            std::make_unique<ReshardingOplogFetcher::Env>(opCtx->getServiceContext(), metrics),
            metadata.getReshardingUUID(),
            metadata.getSourceUUID(),
            // The recipient fetches oplog entries from the donor starting from the largest _id
            // value in the oplog buffer. Otherwise, it starts at fetchTimestamp, which corresponds
            // to {clusterTime: fetchTimestamp, ts: fetchTimestamp} as a resume token value.
            std::move(idToResumeFrom),
            donor,
            myShardId,
            std::move(oplogBufferNss)));
    }

    return oplogFetchers;
}

std::shared_ptr<executor::TaskExecutor> ReshardingDataReplication::_makeOplogFetcherExecutor(
    size_t numDonors) {
    ThreadPool::Limits threadPoolLimits;
    threadPoolLimits.maxThreads = numDonors;
    ThreadPool::Options threadPoolOptions(std::move(threadPoolLimits));

    auto prefix = "ReshardingOplogFetcher"_sd;
    threadPoolOptions.threadNamePrefix = prefix + "-";
    threadPoolOptions.poolName = prefix + "ThreadPool";

    auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(std::move(threadPoolOptions)),
        executor::makeNetworkInterface(prefix + "Network"));

    executor->startup();
    return executor;
}

std::vector<std::unique_ptr<ThreadPool>> ReshardingDataReplication::_makeOplogApplierWorkers(
    size_t numDonors) {
    std::vector<std::unique_ptr<ThreadPool>> oplogApplierWorkers;
    oplogApplierWorkers.reserve(numDonors);

    for (size_t i = 0; i < numDonors; ++i) {
        oplogApplierWorkers.emplace_back(
            repl::makeReplWriterPool(resharding::gReshardingWriterThreadCount,
                                     "ReshardingOplogApplierWorker",
                                     true /* isKillableByStepdown */));
    }

    return oplogApplierWorkers;
}

std::vector<std::unique_ptr<ReshardingOplogApplier>> ReshardingDataReplication::_makeOplogAppliers(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    CommonReshardingMetadata metadata,
    const std::vector<ShardId>& donorShardIds,
    Timestamp fetchTimestamp,
    ChunkManager sourceChunkMgr,
    std::shared_ptr<executor::TaskExecutor> executor,
    const std::vector<NamespaceString>& stashCollections,
    const std::vector<std::unique_ptr<ReshardingOplogFetcher>>& oplogFetchers,
    const std::vector<std::unique_ptr<ThreadPool>>& oplogApplierWorkers) {
    std::vector<std::unique_ptr<ReshardingOplogApplier>> oplogAppliers;
    oplogAppliers.reserve(donorShardIds.size());

    for (size_t i = 0; i < donorShardIds.size(); ++i) {
        auto sourceId = ReshardingSourceId{metadata.getReshardingUUID(), donorShardIds[i]};
        auto idToResumeFrom = resharding::getApplierIdToResumeFrom(opCtx, sourceId, fetchTimestamp);
        invariant((idToResumeFrom >= ReshardingDonorOplogId{fetchTimestamp, fetchTimestamp}));

        const auto& oplogBufferNss =
            getLocalOplogBufferNamespace(metadata.getSourceUUID(), donorShardIds[i]);

        oplogAppliers.emplace_back(std::make_unique<ReshardingOplogApplier>(
            std::make_unique<ReshardingOplogApplier::Env>(opCtx->getServiceContext(), metrics),
            std::move(sourceId),
            oplogBufferNss,
            metadata.getSourceNss(),
            metadata.getSourceUUID(),
            stashCollections,
            i,
            fetchTimestamp,
            // The recipient applies oplog entries from the donor starting from the progress value
            // in progress_applier. Otherwise, it starts at fetchTimestamp, which corresponds to
            // {clusterTime: fetchTimestamp, ts: fetchTimestamp} as a resume token value.
            std::make_unique<ReshardingDonorOplogIterator>(
                oplogBufferNss, std::move(idToResumeFrom), oplogFetchers[i].get()),
            sourceChunkMgr,
            executor,
            oplogApplierWorkers[i].get()));
    }

    return oplogAppliers;
}

std::unique_ptr<ReshardingDataReplicationInterface> ReshardingDataReplication::make(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    CommonReshardingMetadata metadata,
    std::vector<ShardId> donorShardIds,
    Timestamp fetchTimestamp,
    bool cloningDone,
    ShardId myShardId,
    ChunkManager sourceChunkMgr,
    std::shared_ptr<executor::TaskExecutor> executor) {
    std::unique_ptr<ReshardingCollectionCloner> collectionCloner;
    std::vector<std::unique_ptr<ReshardingTxnCloner>> txnCloners;

    if (!cloningDone) {
        collectionCloner = _makeCollectionCloner(metrics, metadata, myShardId, fetchTimestamp);
        txnCloners = _makeTxnCloners(metadata, donorShardIds, fetchTimestamp);
    }

    auto oplogFetchers =
        _makeOplogFetchers(opCtx, metrics, metadata, donorShardIds, fetchTimestamp, myShardId);

    auto oplogFetcherExecutor = _makeOplogFetcherExecutor(donorShardIds.size());
    auto oplogApplierWorkers = _makeOplogApplierWorkers(donorShardIds.size());

    auto stashCollections = resharding::ensureStashCollectionsExist(
        opCtx, sourceChunkMgr, metadata.getSourceUUID(), donorShardIds);

    auto oplogAppliers = _makeOplogAppliers(opCtx,
                                            metrics,
                                            metadata,
                                            donorShardIds,
                                            fetchTimestamp,
                                            std::move(sourceChunkMgr),
                                            std::move(executor),
                                            stashCollections,
                                            oplogFetchers,
                                            oplogApplierWorkers);

    return std::make_unique<ReshardingDataReplication>(std::move(collectionCloner),
                                                       std::move(txnCloners),
                                                       std::move(oplogAppliers),
                                                       std::move(oplogApplierWorkers),
                                                       std::move(oplogFetchers),
                                                       std::move(oplogFetcherExecutor),
                                                       TrustedInitTag{});
}

ReshardingDataReplication::ReshardingDataReplication(
    std::unique_ptr<ReshardingCollectionCloner> collectionCloner,
    std::vector<std::unique_ptr<ReshardingTxnCloner>> txnCloners,
    std::vector<std::unique_ptr<ReshardingOplogApplier>> oplogAppliers,
    std::vector<std::unique_ptr<ThreadPool>> oplogApplierWorkers,
    std::vector<std::unique_ptr<ReshardingOplogFetcher>> oplogFetchers,
    std::shared_ptr<executor::TaskExecutor> oplogFetcherExecutor,
    TrustedInitTag)
    : _collectionCloner{std::move(collectionCloner)},
      _txnCloners{std::move(txnCloners)},
      _oplogAppliers{std::move(oplogAppliers)},
      _oplogApplierWorkers{std::move(oplogApplierWorkers)},
      _oplogFetchers{std::move(oplogFetchers)},
      _oplogFetcherExecutor{std::move(oplogFetcherExecutor)} {}

void ReshardingDataReplication::startOplogApplication() {
    ensureFulfilledPromise(_startOplogApplication);
}

SharedSemiFuture<void> ReshardingDataReplication::awaitCloningDone() {
    return _cloningDone.getFuture();
}

SharedSemiFuture<void> ReshardingDataReplication::awaitConsistentButStale() {
    return _consistentButStale.getFuture();
}

SharedSemiFuture<void> ReshardingDataReplication::awaitStrictlyConsistent() {
    return _strictlyConsistent.getFuture();
}

SemiFuture<void> ReshardingDataReplication::runUntilStrictlyConsistent(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory opCtxFactory,
    Milliseconds minimumOperationDuration) {
    struct ChainContext {
        SharedSemiFuture<void> collectionClonerFuture;
        std::vector<SharedSemiFuture<void>> txnClonerFutures;
        std::vector<SharedSemiFuture<void>> oplogFetcherFutures;
        std::vector<SharedSemiFuture<void>> oplogApplierConsistentButStaleFutures;
        std::vector<SharedSemiFuture<void>> oplogApplierStrictlyConsistentFutures;
    };

    auto chainCtx = std::make_shared<ChainContext>();
    CancellationSource errorSource(cancelToken);

    chainCtx->oplogFetcherFutures = _runOplogFetchers(executor, errorSource.token());
    chainCtx->collectionClonerFuture =
        _runCollectionCloner(executor, cleanupExecutor, errorSource.token(), opCtxFactory);
    chainCtx->txnClonerFutures =
        _runTxnCloners(executor, cleanupExecutor, errorSource.token(), minimumOperationDuration);

    return whenAllSucceed(
               whenAllSucceedOn(chainCtx->oplogFetcherFutures, executor),
               whenAllSucceed(chainCtx->collectionClonerFuture.thenRunOn(executor),
                              whenAllSucceedOn(chainCtx->txnClonerFutures, executor))
                   .thenRunOn(executor)
                   .then([this, executor, errorSource] {
                       _cloningDone.emplaceValue();

                       // We must wait for the RecipientStateMachine to transition to kApplying
                       // before starting to apply any oplog entries.
                       return future_util::withCancellation(_startOplogApplication.getFuture(),
                                                            errorSource.token());
                   })
                   .then([this, executor, chainCtx, errorSource] {
                       chainCtx->oplogApplierConsistentButStaleFutures =
                           _runOplogAppliersUntilConsistentButStale(executor, errorSource.token());

                       return whenAllSucceedOn(chainCtx->oplogApplierConsistentButStaleFutures,
                                               executor);
                   })
                   .then([this, executor, chainCtx, errorSource] {
                       _consistentButStale.emplaceValue();

                       chainCtx->oplogApplierStrictlyConsistentFutures =
                           _runOplogAppliersUntilStrictlyConsistent(executor, errorSource.token());

                       return whenAllSucceedOn(chainCtx->oplogApplierStrictlyConsistentFutures,
                                               executor);
                   }))
        .thenRunOn(executor)
        .onError([this, executor, chainCtx, errorSource](Status originalError) mutable {
            errorSource.cancel();

            ensureFulfilledPromise(_cloningDone, originalError);
            ensureFulfilledPromise(_consistentButStale, originalError);

            return whenAll(
                       whenAllSettledOn(chainCtx->oplogFetcherFutures, executor),
                       chainCtx->collectionClonerFuture.thenRunOn(executor),
                       whenAllSettledOn(chainCtx->txnClonerFutures, executor),
                       whenAllSettledOn(chainCtx->oplogApplierConsistentButStaleFutures, executor),
                       whenAllSettledOn(chainCtx->oplogApplierStrictlyConsistentFutures, executor))
                .ignoreValue()
                .thenRunOn(executor)
                .onCompletion([originalError](auto x) { return originalError; });
        })
        // Fulfilling the _strictlyConsistent promise must be the very last thing in the future
        // chain because RecipientStateMachine, along with its ReshardingDataReplication member,
        // may be destructed immediately afterwards.
        .onCompletion([this](Status status) {
            if (status.isOK()) {
                _strictlyConsistent.emplaceValue();
            } else {
                _strictlyConsistent.setError(status);
            }
        })
        .semi();
}

SharedSemiFuture<void> ReshardingDataReplication::_runCollectionCloner(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory opCtxFactory) {
    return _collectionCloner ? _collectionCloner
                                   ->run(std::move(executor),
                                         std::move(cleanupExecutor),
                                         std::move(cancelToken),
                                         std::move(opCtxFactory))
                                   .share()
                             : makeReadyFutureWith([] {}).share();
}

std::vector<SharedSemiFuture<void>> ReshardingDataReplication::_runTxnCloners(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    Milliseconds minimumOperationDuration) {
    std::vector<SharedSemiFuture<void>> txnClonerFutures;
    txnClonerFutures.reserve(_txnCloners.size());

    for (const auto& txnCloner : _txnCloners) {
        txnClonerFutures.emplace_back(
            executor->sleepFor(minimumOperationDuration, cancelToken)
                .then([executor, cleanupExecutor, cancelToken, txnCloner = txnCloner.get()] {
                    return txnCloner->run(executor, cleanupExecutor, cancelToken);
                })
                .share());
    }

    // ReshardingTxnCloners must complete before the recipient transitions to kApplying to avoid
    // errors caused by donor shards unpinning the fetchTimestamp.
    return txnClonerFutures;
}

std::vector<SharedSemiFuture<void>> ReshardingDataReplication::_runOplogFetchers(
    std::shared_ptr<executor::TaskExecutor> executor, CancellationToken cancelToken) {
    std::vector<SharedSemiFuture<void>> oplogFetcherFutures;
    oplogFetcherFutures.reserve(_oplogFetchers.size());

    for (const auto& fetcher : _oplogFetchers) {
        oplogFetcherFutures.emplace_back(
            fetcher->schedule(_oplogFetcherExecutor, cancelToken).share());
    }

    return oplogFetcherFutures;
}

std::vector<SharedSemiFuture<void>>
ReshardingDataReplication::_runOplogAppliersUntilConsistentButStale(
    std::shared_ptr<executor::TaskExecutor> executor, CancellationToken cancelToken) {
    std::vector<SharedSemiFuture<void>> oplogApplierFutures;
    oplogApplierFutures.reserve(_oplogAppliers.size());

    for (const auto& applier : _oplogAppliers) {
        oplogApplierFutures.emplace_back(applier->applyUntilCloneFinishedTs(cancelToken).share());
    }

    return oplogApplierFutures;
}

std::vector<SharedSemiFuture<void>>
ReshardingDataReplication::_runOplogAppliersUntilStrictlyConsistent(
    std::shared_ptr<executor::TaskExecutor> executor, CancellationToken cancelToken) {
    std::vector<SharedSemiFuture<void>> oplogApplierFutures;
    oplogApplierFutures.reserve(_oplogAppliers.size());

    for (const auto& applier : _oplogAppliers) {
        oplogApplierFutures.emplace_back(applier->applyUntilDone(cancelToken).share());
    }

    return oplogApplierFutures;
}

void ReshardingDataReplication::shutdown() {
    _oplogFetcherExecutor->shutdown();

    for (const auto& worker : _oplogApplierWorkers) {
        worker->shutdown();
    }
}

}  // namespace mongo
