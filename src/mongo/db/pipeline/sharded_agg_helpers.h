/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/query/owned_remote_cursor.h"

namespace mongo {
class CachedCollectionRoutingInfo;

namespace sharded_agg_helpers {

/**
 * Represents the two halves of a pipeline that will execute in a sharded cluster. 'shardsPipeline'
 * will execute in parallel on each shard, and 'mergePipeline' will execute on the merge host -
 * either one of the shards or a mongos.
 */
struct SplitPipeline {
    SplitPipeline(std::unique_ptr<Pipeline, PipelineDeleter> shardsPipeline,
                  std::unique_ptr<Pipeline, PipelineDeleter> mergePipeline,
                  boost::optional<BSONObj> shardCursorsSortSpec)
        : shardsPipeline(std::move(shardsPipeline)),
          mergePipeline(std::move(mergePipeline)),
          shardCursorsSortSpec(std::move(shardCursorsSortSpec)) {}

    std::unique_ptr<Pipeline, PipelineDeleter> shardsPipeline;
    std::unique_ptr<Pipeline, PipelineDeleter> mergePipeline;

    // If set, the cursors from the shards are expected to be sorted according to this spec, and to
    // have populated a "$sortKey" metadata field which can be used to compare the results.
    boost::optional<BSONObj> shardCursorsSortSpec;
};

struct ShardedExchangePolicy {
    // The exchange specification that will be sent to shards as part of the aggregate command.
    // It will be used by producers to determine how to distribute documents to consumers.
    ExchangeSpec exchangeSpec;

    // Shards that will run the consumer part of the exchange.
    std::vector<ShardId> consumerShards;
};

struct DispatchShardPipelineResults {
    // True if this pipeline was split, and the second half of the pipeline needs to be run on
    // the primary shard for the database.
    bool needsPrimaryShardMerge;

    // Populated if this *is not* an explain, this vector represents the cursors on the remote
    // shards.
    std::vector<OwnedRemoteCursor> remoteCursors;

    // Populated if this *is* an explain, this vector represents the results from each shard.
    std::vector<AsyncRequestsSender::Response> remoteExplainOutput;

    // The split version of the pipeline if more than one shard was targeted, otherwise
    // boost::none.
    boost::optional<SplitPipeline> splitPipeline;

    // If the pipeline targeted a single shard, this is the pipeline to run on that shard.
    std::unique_ptr<Pipeline, PipelineDeleter> pipelineForSingleShard;

    // The command object to send to the targeted shards.
    BSONObj commandForTargetedShards;

    // How many exchange producers are running the shard part of splitPipeline.
    size_t numProducers;

    // The exchange specification if the query can run with the exchange otherwise boost::none.
    boost::optional<ShardedExchangePolicy> exchangeSpec;
};

/**
 * If the merging pipeline is eligible for an $exchange merge optimization, returns the information
 * required to set that up.
 */
boost::optional<ShardedExchangePolicy> checkIfEligibleForExchange(OperationContext* opCtx,
                                                                  const Pipeline* mergePipeline);

/**
 * Split the current Pipeline into a Pipeline for each shard, and a Pipeline that combines the
 * results within a merging process. This call also performs optimizations with the aim of reducing
 * computing time and network traffic when a pipeline has been split into two pieces.
 *
 * The 'mergePipeline' returned as part of the SplitPipeline here is not ready to execute until the
 * 'shardsPipeline' has been sent to the shards and cursors have been established. Once cursors have
 * been established, the merge pipeline can be made executable by calling 'addMergeCursorsSource()'
 */
SplitPipeline splitPipeline(std::unique_ptr<Pipeline, PipelineDeleter> pipeline);

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and
 * the pipeline that will need to be executed to merge the results from the remotes. If a stale
 * shard version is encountered, refreshes the routing table and tries again.
 */
DispatchShardPipelineResults dispatchShardPipeline(
    Document serializedCommand,
    bool hasChangeStream,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline);

BSONObj createPassthroughCommandForShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Document serializedCommand,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity,
    const boost::optional<RuntimeConstants>& constants,
    Pipeline* pipeline,
    BSONObj collationObj);

BSONObj createCommandForTargetedShards(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       Document serializedCommand,
                                       const SplitPipeline& splitPipeline,
                                       const boost::optional<ShardedExchangePolicy> exchangeSpec,
                                       bool needsMerge);

/**
 * Creates a new DocumentSourceMergeCursors from the provided 'remoteCursors' and adds it to the
 * front of 'mergePipeline'.
 */
void addMergeCursorsSource(Pipeline* mergePipeline,
                           BSONObj cmdSentToShards,
                           std::vector<OwnedRemoteCursor> ownedCursors,
                           const std::vector<ShardId>& targetedShards,
                           boost::optional<BSONObj> shardCursorsSortSpec,
                           bool hasChangeStream);

/**
 * Targets the shards with an aggregation command built from `ownedPipeline` and explain set to
 * true. Returns a BSONObj of the form {"pipeline": {<pipelineExplainOutput>}}.
 */
BSONObj targetShardsForExplain(Pipeline* ownedPipeline);

/**
 * Appends the explain output of `dispatchResults` to `result`.
 */
Status appendExplainResults(DispatchShardPipelineResults&& dispatchResults,
                            const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                            BSONObjBuilder* result);

/**
 * Returns the proper routing table to use for targeting shards: either a historical routing table
 * based on the global read timestamp if there is an active transaction with snapshot level read
 * concern or the latest routing table otherwise.
 *
 * Returns 'ShardNotFound' or 'NamespaceNotFound' if there are no shards in the cluster or if
 * collection 'execNss' does not exist, respectively.
 */
StatusWith<CachedCollectionRoutingInfo> getExecutionNsRoutingInfo(OperationContext* opCtx,
                                                                  const NamespaceString& execNss);

/**
 * Returns true if an aggregation over 'nss' must run on all shards.
 */
bool mustRunOnAllShards(const NamespaceString& nss, bool hasChangeStream);

/**
 * Retrieves the desired retry policy based on whether the default writeConcern is set on 'opCtx'.
 */
Shard::RetryPolicy getDesiredRetryPolicy(OperationContext* opCtx);

/**
 * Uses sharded_agg_helpers to split the pipeline and dispatch half to the shards, leaving the
 * merging half executing in this process after attaching a $mergeCursors. Will retry on network
 * errors and also on StaleConfig errors to avoid restarting the entire operation.
 */
std::unique_ptr<Pipeline, PipelineDeleter> attachCursorToPipeline(Pipeline* ownedPipeline,
                                                                  bool allowTargetingShards);

/**
 * Adds a log message with the given message. Simple helper to avoid defining the log component in a
 * header file.
 */
void logFailedRetryAttempt(StringData taskDescription, const DBException&);

/**
 * A retry loop which handles errors in ErrorCategory::StaleShardVersionError. When such an error is
 * encountered, the CatalogCache is marked for refresh and 'callback' is retried. When retried,
 * 'callback' will trigger a refresh of the CatalogCache and block until it's done when it next
 * consults the CatalogCache.
 */
template <typename F>
auto shardVersionRetry(OperationContext* opCtx,
                       CatalogCache* catalogCache,
                       NamespaceString nss,
                       StringData taskDescription,
                       F&& callbackFn) {
    size_t numAttempts = 0;
    auto logAndTestMaxRetries = [&numAttempts, taskDescription](auto& exception) {
        if (++numAttempts <= kMaxNumStaleVersionRetries) {
            logFailedRetryAttempt(taskDescription, exception);
            return true;
        }
        exception.addContext(str::stream()
                             << "Exceeded maximum number of " << kMaxNumStaleVersionRetries
                             << " retries attempting " << taskDescription);
        return false;
    };
    while (true) {
        catalogCache->setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, numAttempts);
        try {
            return callbackFn();
        } catch (ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
            invariant(ex->getDb() == nss.db(),
                      str::stream() << "StaleDbVersion error on unexpected database. Expected "
                                    << nss.db() << ", received " << ex->getDb());

            // If the database version is stale, refresh its entry in the catalog cache.
            Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(ex->getDb(),
                                                                     ex->getVersionWanted());

            if (!logAndTestMaxRetries(ex)) {
                throw;
            }
        } catch (ExceptionForCat<ErrorCategory::StaleShardVersionError>& e) {
            // If the exception provides a shardId, add it to the set of shards requiring a refresh.
            // If the cache currently considers the collection to be unsharded, this will trigger an
            // epoch refresh. If no shard is provided, then the epoch is stale and we must refresh.
            if (auto staleInfo = e.extraInfo<StaleConfigInfo>()) {
                invariant(staleInfo->getNss() == nss,
                          str::stream() << "StaleConfig error on unexpected namespace. Expected "
                                        << nss << ", received " << staleInfo->getNss());
                catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                    opCtx,
                    nss,
                    staleInfo->getVersionWanted(),
                    staleInfo->getVersionReceived(),
                    staleInfo->getShardId());
            } else {
                catalogCache->onEpochChange(nss);
            }
            if (!logAndTestMaxRetries(e)) {
                throw;
            }
        } catch (ExceptionFor<ErrorCodes::ShardInvalidatedForTargeting>& e) {
            if (!logAndTestMaxRetries(e)) {
                throw;
            }
        }
    }
}
}  // namespace sharded_agg_helpers
}  // namespace mongo
