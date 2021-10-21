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

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "mongo/bson/timestamp.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"

namespace mongo {

class OperationContext;
class ReshardingOplogApplier;
class ReshardingCollectionCloner;
class ReshardingMetrics;
class ReshardingOplogFetcher;
class ReshardingTxnCloner;
class ServiceContext;
class ThreadPool;

namespace executor {

class TaskExecutor;

}  // namespace executor

/**
 * Manages the full sequence of data replication in resharding on the recipient.
 *
 *   - Cloning the collection being resharded.
 *   - Cloning the config.transactions records from before the resharding operation started.
 *   - Fetching any oplog entries affecting the collection being resharded.
 *   - Applying the fetched oplog entries.
 */
class ReshardingDataReplicationInterface {
public:
    virtual ~ReshardingDataReplicationInterface() = default;

    /**
     * Begins the data replication procedure and runs it to completion.
     *
     *   - Immediately starts cloning the collection being resharded.
     *   - Immediately starts fetching any oplog entries affecting the collection being resharded.
     *   - After minimumOperationDuration milliseconds, starts cloning the config.transactions
     *     records from before the resharding operation started.
     *   - After cloning both the collection being resharded and the config.transactions records
     *     from before the resharding operation started, and after startOplogApplication() has been
     *     called, starts applying the fetched oplog entries.
     *
     * This function returns a future that becomes ready when either
     *   (a) the recipient has applied the final resharding oplog entry from every donor shard, or
     *   (b) the recipient has encountered an operation-fatal error.
     *
     * The caller must take care to wait on the returned future or a future returned by
     * awaitStrictlyConsistent() to guarantee that all of the data replication components have
     * stopped running in both the success and failure cases.
     */
    virtual SemiFuture<void> runUntilStrictlyConsistent(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory,
        const mongo::Date_t& startConfigTxnCloneTime) = 0;

    /**
     * Releases the barrier to allow the fetched oplog entries to be applied.
     *
     * This method exists on ReshardingDataReplicationInterface to allow the RecipientStateMachine
     * to transition from kCloning to kApplying before the fetched oplog entries start to be
     * applied.
     *
     * This function is safe to be called multiple times in sequence but must not be called
     * concurrently by another thread.
     */
    virtual void startOplogApplication() = 0;

    /**
     * Returns a future that becomes ready when either
     *   (a) the recipient has finished cloning both the collection being resharded and the
     *       config.transactions records from before the resharding operation started, or
     *   (b) the recipient has encountered an operation-fatal error.
     */
    virtual SharedSemiFuture<void> awaitCloningDone() = 0;

    /**
     * Returns a future that becomes ready when either
     *   (a) the recipient has applied the final resharding oplog entry from every donor shard, or
     *   (b) the recipient has encountered an operation-fatal error.
     */
    virtual SharedSemiFuture<void> awaitStrictlyConsistent() = 0;

    virtual void shutdown() = 0;
};

class ReshardingDataReplication : public ReshardingDataReplicationInterface {
private:
    struct TrustedInitTag {};

public:
    static std::unique_ptr<ReshardingDataReplicationInterface> make(
        OperationContext* opCtx,
        ReshardingMetrics* metrics,
        CommonReshardingMetadata metadata,
        const std::vector<DonorShardFetchTimestamp>& donorShards,
        Timestamp cloneTimestamp,
        bool cloningDone,
        ShardId myShardId,
        ChunkManager sourceChunkMgr);

    // The TrustedInitTag being a private class makes this constructor effectively private. However,
    // it needs to technically be a public constructor for std::make_unique to be able to call it.
    // This C++ technique is known as the passkey idiom. ReshardingDataReplication::make() is the
    // entry point for constructing instances of ReshardingDataReplication.
    ReshardingDataReplication(std::unique_ptr<ReshardingCollectionCloner> collectionCloner,
                              std::vector<std::unique_ptr<ReshardingTxnCloner>> txnCloners,
                              std::vector<std::unique_ptr<ReshardingOplogFetcher>> oplogFetchers,
                              std::shared_ptr<executor::TaskExecutor> oplogFetcherExecutor,
                              std::vector<std::unique_ptr<ReshardingOplogApplier>> oplogAppliers,
                              TrustedInitTag);

    SemiFuture<void> runUntilStrictlyConsistent(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory,
        const mongo::Date_t& startConfigTxnCloneTime) override;

    void startOplogApplication() override;

    SharedSemiFuture<void> awaitCloningDone() override;

    SharedSemiFuture<void> awaitStrictlyConsistent() override;

    void shutdown() override;

    // The following methods are called by ReshardingDataReplication::make() and only exposed
    // publicly for unit-testing purposes.

    static std::vector<NamespaceString> ensureStashCollectionsExist(
        OperationContext* opCtx,
        const ChunkManager& sourceChunkMgr,
        const std::vector<DonorShardFetchTimestamp>& donorShards);

    static ReshardingDonorOplogId getOplogFetcherResumeId(OperationContext* opCtx,
                                                          const UUID& reshardingUUID,
                                                          const NamespaceString& oplogBufferNss,
                                                          Timestamp minFetchTimestamp);

    static ReshardingDonorOplogId getOplogApplierResumeId(OperationContext* opCtx,
                                                          const ReshardingSourceId& sourceId,
                                                          Timestamp minFetchTimestamp);

private:
    static std::unique_ptr<ReshardingCollectionCloner> _makeCollectionCloner(
        ReshardingMetrics* metrics,
        const CommonReshardingMetadata& metadata,
        const ShardId& myShardId,
        Timestamp cloneTimestamp);

    static std::vector<std::unique_ptr<ReshardingTxnCloner>> _makeTxnCloners(
        const CommonReshardingMetadata& metadata,
        const std::vector<DonorShardFetchTimestamp>& donorShards);

    static std::vector<std::unique_ptr<ReshardingOplogFetcher>> _makeOplogFetchers(
        OperationContext* opCtx,
        ReshardingMetrics* metrics,
        const CommonReshardingMetadata& metadata,
        const std::vector<DonorShardFetchTimestamp>& donorShards,
        const ShardId& myShardId);

    static std::shared_ptr<executor::TaskExecutor> _makeOplogFetcherExecutor(size_t numDonors);

    static std::vector<std::unique_ptr<ReshardingOplogApplier>> _makeOplogAppliers(
        OperationContext* opCtx,
        ReshardingMetrics* metrics,
        const CommonReshardingMetadata& metadata,
        const std::vector<DonorShardFetchTimestamp>& donorShards,
        Timestamp cloneTimestamp,
        ChunkManager sourceChunkMgr,
        const std::vector<NamespaceString>& stashCollections,
        const std::vector<std::unique_ptr<ReshardingOplogFetcher>>& oplogFetchers);

    SharedSemiFuture<void> _runCollectionCloner(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory);

    std::vector<SharedSemiFuture<void>> _runTxnCloners(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory,
        const mongo::Date_t& startConfigTxnCloneTime);

    std::vector<SharedSemiFuture<void>> _runOplogFetchers(
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory);

    std::vector<SharedSemiFuture<void>> _runOplogAppliers(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory);

    // _collectionCloner is left as nullptr when make() is called with cloningDone=true.
    const std::unique_ptr<ReshardingCollectionCloner> _collectionCloner;

    // _txnCloners is left as an empty vector when make() is called with cloningDone=true.
    const std::vector<std::unique_ptr<ReshardingTxnCloner>> _txnCloners;

    const std::vector<std::unique_ptr<ReshardingOplogFetcher>> _oplogFetchers;
    const std::shared_ptr<executor::TaskExecutor> _oplogFetcherExecutor;

    const std::vector<std::unique_ptr<ReshardingOplogApplier>> _oplogAppliers;

    // Promise fulfilled by startOplogApplication() to signal that oplog application can begin.
    SharedPromise<void> _startOplogApplication;

    SharedPromise<void> _cloningDone;
    SharedPromise<void> _strictlyConsistent;
};

using ReshardingDataReplicationFactory = unique_function<decltype(ReshardingDataReplication::make)>;

}  // namespace mongo
