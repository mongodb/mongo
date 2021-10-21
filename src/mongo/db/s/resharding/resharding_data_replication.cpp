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
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"
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
 *
 * This function is not thread-safe and must not be called concurrently with the promise being
 * fulfilled by another thread.
 */
void ensureFulfilledPromise(SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

/**
 * Fulfills the promise with an error if it isn't already fulfilled. Does nothing otherwise.
 *
 * This function is not thread-safe and must not be called concurrently with the promise being
 * fulfilled by another thread.
 */
void ensureFulfilledPromise(SharedPromise<void>& sp, Status error) {
    if (!sp.getFuture().isReady()) {
        sp.setError(error);
    }
}

}  // namespace

std::unique_ptr<ReshardingCollectionCloner> ReshardingDataReplication::_makeCollectionCloner(
    ReshardingMetrics* metrics,
    const CommonReshardingMetadata& metadata,
    const ShardId& myShardId,
    Timestamp cloneTimestamp) {
    return std::make_unique<ReshardingCollectionCloner>(
        std::make_unique<ReshardingCollectionCloner::Env>(metrics),
        ShardKeyPattern{metadata.getReshardingKey()},
        metadata.getSourceNss(),
        metadata.getSourceUUID(),
        myShardId,
        cloneTimestamp,
        metadata.getTempReshardingNss());
}

std::vector<std::unique_ptr<ReshardingTxnCloner>> ReshardingDataReplication::_makeTxnCloners(
    const CommonReshardingMetadata& metadata,
    const std::vector<DonorShardFetchTimestamp>& donorShards) {
    std::vector<std::unique_ptr<ReshardingTxnCloner>> txnCloners;
    txnCloners.reserve(donorShards.size());

    for (const auto& donor : donorShards) {
        txnCloners.emplace_back(std::make_unique<ReshardingTxnCloner>(
            ReshardingSourceId(metadata.getReshardingUUID(), donor.getShardId()),
            *donor.getMinFetchTimestamp()));
    }

    return txnCloners;
}

std::vector<std::unique_ptr<ReshardingOplogFetcher>> ReshardingDataReplication::_makeOplogFetchers(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const CommonReshardingMetadata& metadata,
    const std::vector<DonorShardFetchTimestamp>& donorShards,
    const ShardId& myShardId) {
    std::vector<std::unique_ptr<ReshardingOplogFetcher>> oplogFetchers;
    oplogFetchers.reserve(donorShards.size());

    for (const auto& donor : donorShards) {
        auto oplogBufferNss =
            getLocalOplogBufferNamespace(metadata.getSourceUUID(), donor.getShardId());
        auto minFetchTimestamp = *donor.getMinFetchTimestamp();
        auto idToResumeFrom = getOplogFetcherResumeId(
            opCtx, metadata.getReshardingUUID(), oplogBufferNss, minFetchTimestamp);
        invariant((idToResumeFrom >= ReshardingDonorOplogId{minFetchTimestamp, minFetchTimestamp}));

        oplogFetchers.emplace_back(std::make_unique<ReshardingOplogFetcher>(
            std::make_unique<ReshardingOplogFetcher::Env>(opCtx->getServiceContext(), metrics),
            metadata.getReshardingUUID(),
            metadata.getSourceUUID(),
            // The recipient fetches oplog entries from the donor starting from the largest _id
            // value in the oplog buffer. Otherwise, it starts at minFetchTimestamp, which
            // corresponds to {clusterTime: minFetchTimestamp, ts: minFetchTimestamp} as a resume
            // token value.
            std::move(idToResumeFrom),
            donor.getShardId(),
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

std::vector<std::unique_ptr<ReshardingOplogApplier>> ReshardingDataReplication::_makeOplogAppliers(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const CommonReshardingMetadata& metadata,
    const std::vector<DonorShardFetchTimestamp>& donorShards,
    Timestamp cloneTimestamp,
    ChunkManager sourceChunkMgr,
    const std::vector<NamespaceString>& stashCollections,
    const std::vector<std::unique_ptr<ReshardingOplogFetcher>>& oplogFetchers) {
    std::vector<std::unique_ptr<ReshardingOplogApplier>> oplogAppliers;
    oplogAppliers.reserve(donorShards.size());

    for (size_t i = 0; i < donorShards.size(); ++i) {
        auto sourceId =
            ReshardingSourceId{metadata.getReshardingUUID(), donorShards[i].getShardId()};
        auto minFetchTimestamp = *donorShards[i].getMinFetchTimestamp();
        auto idToResumeFrom = getOplogApplierResumeId(opCtx, sourceId, minFetchTimestamp);
        invariant((idToResumeFrom >= ReshardingDonorOplogId{minFetchTimestamp, minFetchTimestamp}));

        const auto& oplogBufferNss =
            getLocalOplogBufferNamespace(metadata.getSourceUUID(), donorShards[i].getShardId());

        oplogAppliers.emplace_back(std::make_unique<ReshardingOplogApplier>(
            std::make_unique<ReshardingOplogApplier::Env>(opCtx->getServiceContext(), metrics),
            std::move(sourceId),
            metadata.getTempReshardingNss(),
            stashCollections,
            i,
            sourceChunkMgr,
            // The recipient applies oplog entries from the donor starting from the progress value
            // in progress_applier. Otherwise, it starts at minFetchTimestamp, which corresponds to
            // {clusterTime: minFetchTimestamp, ts: minFetchTimestamp} as a resume token value.
            std::make_unique<ReshardingDonorOplogIterator>(
                oplogBufferNss, std::move(idToResumeFrom), oplogFetchers[i].get())));
    }

    return oplogAppliers;
}

std::unique_ptr<ReshardingDataReplicationInterface> ReshardingDataReplication::make(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    CommonReshardingMetadata metadata,
    const std::vector<DonorShardFetchTimestamp>& donorShards,
    Timestamp cloneTimestamp,
    bool cloningDone,
    ShardId myShardId,
    ChunkManager sourceChunkMgr) {
    std::unique_ptr<ReshardingCollectionCloner> collectionCloner;
    std::vector<std::unique_ptr<ReshardingTxnCloner>> txnCloners;

    if (!cloningDone) {
        collectionCloner = _makeCollectionCloner(metrics, metadata, myShardId, cloneTimestamp);
        txnCloners = _makeTxnCloners(metadata, donorShards);
    }

    auto oplogFetchers = _makeOplogFetchers(opCtx, metrics, metadata, donorShards, myShardId);

    auto oplogFetcherExecutor = _makeOplogFetcherExecutor(donorShards.size());

    auto stashCollections = ensureStashCollectionsExist(opCtx, sourceChunkMgr, donorShards);
    auto oplogAppliers = _makeOplogAppliers(opCtx,
                                            metrics,
                                            metadata,
                                            donorShards,
                                            cloneTimestamp,
                                            std::move(sourceChunkMgr),
                                            stashCollections,
                                            oplogFetchers);

    return std::make_unique<ReshardingDataReplication>(std::move(collectionCloner),
                                                       std::move(txnCloners),
                                                       std::move(oplogFetchers),
                                                       std::move(oplogFetcherExecutor),
                                                       std::move(oplogAppliers),
                                                       TrustedInitTag{});
}

ReshardingDataReplication::ReshardingDataReplication(
    std::unique_ptr<ReshardingCollectionCloner> collectionCloner,
    std::vector<std::unique_ptr<ReshardingTxnCloner>> txnCloners,
    std::vector<std::unique_ptr<ReshardingOplogFetcher>> oplogFetchers,
    std::shared_ptr<executor::TaskExecutor> oplogFetcherExecutor,
    std::vector<std::unique_ptr<ReshardingOplogApplier>> oplogAppliers,
    TrustedInitTag)
    : _collectionCloner{std::move(collectionCloner)},
      _txnCloners{std::move(txnCloners)},
      _oplogFetchers{std::move(oplogFetchers)},
      _oplogFetcherExecutor{std::move(oplogFetcherExecutor)},
      _oplogAppliers{std::move(oplogAppliers)} {}

void ReshardingDataReplication::startOplogApplication() {
    ensureFulfilledPromise(_startOplogApplication);
}

SharedSemiFuture<void> ReshardingDataReplication::awaitCloningDone() {
    return _cloningDone.getFuture();
}

SharedSemiFuture<void> ReshardingDataReplication::awaitStrictlyConsistent() {
    return _strictlyConsistent.getFuture();
}

SemiFuture<void> ReshardingDataReplication::runUntilStrictlyConsistent(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory opCtxFactory,
    const mongo::Date_t& startConfigTxnCloneTime) {
    CancellationSource errorSource(cancelToken);

    auto oplogFetcherFutures = _runOplogFetchers(executor, errorSource.token(), opCtxFactory);

    auto collectionClonerFuture =
        _runCollectionCloner(executor, cleanupExecutor, errorSource.token(), opCtxFactory);

    auto txnClonerFutures = _runTxnCloners(
        executor, cleanupExecutor, errorSource.token(), opCtxFactory, startConfigTxnCloneTime);

    auto fulfillCloningDoneFuture =
        whenAllSucceed(collectionClonerFuture.thenRunOn(executor),
                       resharding::whenAllSucceedOn(txnClonerFutures, executor))
            .thenRunOn(executor)
            .then([this] { _cloningDone.emplaceValue(); })
            .share();

    // Calling _runOplogAppliers() won't actually immediately start performing oplog application.
    // Only after the _startOplogApplication promise is fulfilled will oplog application begin.
    auto oplogApplierFutures =
        _runOplogAppliers(executor, cleanupExecutor, errorSource.token(), opCtxFactory);

    // We must additionally wait for fulfillCloningDoneFuture to become ready to ensure their
    // corresponding promises aren't being fulfilled while the .onCompletion() is running.
    std::vector<SharedSemiFuture<void>> allFutures;
    allFutures.reserve(2 + oplogFetcherFutures.size() + txnClonerFutures.size() +
                       oplogApplierFutures.size());

    for (const auto& futureList : {oplogFetcherFutures,
                                   {collectionClonerFuture},
                                   txnClonerFutures,
                                   {fulfillCloningDoneFuture},
                                   oplogApplierFutures}) {
        for (const auto& future : futureList) {
            allFutures.emplace_back(future);
        }
    }

    return resharding::cancelWhenAnyErrorThenQuiesce(allFutures, executor, errorSource)
        // Fulfilling the _strictlyConsistent promise must be the very last thing in the future
        // chain because RecipientStateMachine, along with its ReshardingDataReplication member,
        // may be destructed immediately afterwards.
        .onCompletion([this](Status status) {
            if (status.isOK()) {
                invariant(_cloningDone.getFuture().isReady());
                _strictlyConsistent.emplaceValue();
            } else {
                ensureFulfilledPromise(_cloningDone, status);
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
    CancelableOperationContextFactory opCtxFactory,
    const mongo::Date_t& startConfigTxnCloneTime) {
    std::vector<SharedSemiFuture<void>> txnClonerFutures;
    txnClonerFutures.reserve(_txnCloners.size());

    for (const auto& txnCloner : _txnCloners) {
        txnClonerFutures.emplace_back(
            executor->sleepUntil(startConfigTxnCloneTime, cancelToken)
                .then([executor,
                       cleanupExecutor,
                       cancelToken,
                       opCtxFactory,
                       txnCloner = txnCloner.get()] {
                    return txnCloner->run(executor, cleanupExecutor, cancelToken, opCtxFactory);
                })
                .share());
    }

    // ReshardingTxnCloners must complete before the recipient transitions to kApplying to avoid
    // errors caused by donor shards unpinning their minFetchTimestamp.
    return txnClonerFutures;
}

std::vector<SharedSemiFuture<void>> ReshardingDataReplication::_runOplogFetchers(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory opCtxFactory) {
    std::vector<SharedSemiFuture<void>> oplogFetcherFutures;
    oplogFetcherFutures.reserve(_oplogFetchers.size());

    for (const auto& fetcher : _oplogFetchers) {
        oplogFetcherFutures.emplace_back(
            fetcher->schedule(_oplogFetcherExecutor, cancelToken, opCtxFactory).share());
    }

    return oplogFetcherFutures;
}

std::vector<SharedSemiFuture<void>> ReshardingDataReplication::_runOplogAppliers(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory opCtxFactory) {
    std::vector<SharedSemiFuture<void>> oplogApplierFutures;
    oplogApplierFutures.reserve(_oplogAppliers.size());

    for (const auto& applier : _oplogAppliers) {
        // We must wait for the RecipientStateMachine to transition to kApplying before starting to
        // apply any oplog entries.
        oplogApplierFutures.emplace_back(
            future_util::withCancellation(_startOplogApplication.getFuture(), cancelToken)
                .thenRunOn(executor)
                .then([applier = applier.get(),
                       executor,
                       cleanupExecutor,
                       cancelToken,
                       opCtxFactory] {
                    return applier->run(executor, cleanupExecutor, cancelToken, opCtxFactory);
                })
                .share());
    }

    return oplogApplierFutures;
}

void ReshardingDataReplication::shutdown() {
    _oplogFetcherExecutor->shutdown();
}

std::vector<NamespaceString> ReshardingDataReplication::ensureStashCollectionsExist(
    OperationContext* opCtx,
    const ChunkManager& sourceChunkMgr,
    const std::vector<DonorShardFetchTimestamp>& donorShards) {
    // Use the same collation for the stash collections as the temporary resharding collection
    // (which is also the same as the collation for the collection being resharded).
    CollectionOptions options;
    if (auto collator = sourceChunkMgr.getDefaultCollator()) {
        options.collation = collator->getSpec().toBSON();
    }

    std::vector<NamespaceString> stashCollections;
    stashCollections.reserve(donorShards.size());

    for (const auto& donor : donorShards) {
        stashCollections.emplace_back(ReshardingOplogApplier::ensureStashCollectionExists(
            opCtx, *sourceChunkMgr.getUUID(), donor.getShardId(), options));
    }

    return stashCollections;
}

ReshardingDonorOplogId ReshardingDataReplication::getOplogFetcherResumeId(
    OperationContext* opCtx,
    const UUID& reshardingUUID,
    const NamespaceString& oplogBufferNss,
    Timestamp minFetchTimestamp) {
    invariant(!opCtx->lockState()->isLocked());

    AutoGetCollection coll(opCtx, oplogBufferNss, MODE_IS);
    if (coll) {
        auto highestOplogBufferId =
            resharding::data_copy::findDocWithHighestInsertedId(opCtx, *coll);

        if (highestOplogBufferId) {
            auto oplogEntry = repl::OplogEntry{highestOplogBufferId->toBson()};
            if (isFinalOplog(oplogEntry, reshardingUUID)) {
                return ReshardingOplogFetcher::kFinalOpAlreadyFetched;
            }

            return ReshardingDonorOplogId::parse({"getOplogFetcherResumeId"},
                                                 oplogEntry.get_id()->getDocument().toBson());
        }
    }

    return ReshardingDonorOplogId{minFetchTimestamp, minFetchTimestamp};
}

ReshardingDonorOplogId ReshardingDataReplication::getOplogApplierResumeId(
    OperationContext* opCtx, const ReshardingSourceId& sourceId, Timestamp minFetchTimestamp) {
    auto applierProgress = ReshardingOplogApplier::checkStoredProgress(opCtx, sourceId);
    return applierProgress ? applierProgress->getProgress()
                           : ReshardingDonorOplogId{minFetchTimestamp, minFetchTimestamp};
}

}  // namespace mongo
