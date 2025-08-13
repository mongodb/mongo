/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/s/resharding/resharding_collection_cloner.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"

#include <cinttypes>
#include <mutex>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

// For simulating errors while cloning. Takes "donorShard" as data.
MONGO_FAIL_POINT_DEFINE(reshardingCollectionClonerAbort);

MONGO_FAIL_POINT_DEFINE(reshardingCollectionClonerPauseBeforeAttempt);
MONGO_FAIL_POINT_DEFINE(reshardingCollectionClonerShouldFailWithStaleConfig);
MONGO_FAIL_POINT_DEFINE(reshardingCollectionClonerPauseBeforeWriteNaturalOrder);

namespace mongo {
namespace {

bool collectionHasSimpleCollation(OperationContext* opCtx, const NamespaceString& nss) {
    const auto cri =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Expected collection '" << nss.toStringForErrorMsg()
                          << "' to be tracked",
            cri.hasRoutingTable());
    return !cri.getChunkManager().getDefaultCollator();
}

}  // namespace

ReshardingCollectionCloner::ReshardingCollectionCloner(ReshardingMetrics* metrics,
                                                       const UUID& reshardingUUID,
                                                       ShardKeyPattern newShardKeyPattern,
                                                       NamespaceString sourceNss,
                                                       const UUID& sourceUUID,
                                                       ShardId recipientShard,
                                                       Timestamp atClusterTime,
                                                       NamespaceString outputNss,
                                                       bool storeProgress,
                                                       bool relaxed)
    : _metrics(metrics),
      _reshardingUUID(reshardingUUID),
      _newShardKeyPattern(std::move(newShardKeyPattern)),
      _sourceNss(std::move(sourceNss)),
      _sourceUUID(sourceUUID),
      _recipientShard(std::move(recipientShard)),
      _atClusterTime(atClusterTime),
      _outputNss(std::move(outputNss)),
      _storeProgress(storeProgress),
      _relaxed(std::move(relaxed)) {}

std::pair<std::vector<BSONObj>, boost::intrusive_ptr<ExpressionContext>>
ReshardingCollectionCloner::makeRawNaturalOrderPipeline(
    OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface) {
    // Assume that the input collection isn't a view. The collectionUUID parameter to
    // the aggregate would enforce this anyway.
    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[_sourceNss] = {_sourceNss, std::vector<BSONObj>{}};

    // Assume that the config.cache.chunks collection isn't a view either.
    auto tempNss = resharding::constructTemporaryReshardingNss(_sourceNss, _sourceUUID);
    auto tempCacheChunksNss = NamespaceString::makeGlobalConfigCollection(
        "cache.chunks." +
        NamespaceStringUtil::serialize(tempNss, SerializationContext::stateDefault()));
    resolvedNamespaces[tempCacheChunksNss] = {tempCacheChunksNss, std::vector<BSONObj>{}};
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .mongoProcessInterface(std::move(mongoProcessInterface))
                      .ns(_sourceNss)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .collUUID(_sourceUUID)
                      .build();
    std::vector<BSONObj> rawPipeline;
    auto keyPattern = ShardKeyPattern(_newShardKeyPattern.getKeyPattern()).toBSON();
    rawPipeline.emplace_back(BSON(
        DocumentSourceReshardingOwnershipMatch::kStageName
        << BSON("recipientShardId"
                << _recipientShard << "reshardingKey" << keyPattern
                << "temporaryReshardingNamespace"
                << NamespaceStringUtil::serialize(tempNss, SerializationContext::stateDefault()))));

    return std::make_pair(std::move(rawPipeline), std::move(expCtx));
}

class ReshardingCloneFetcher {
public:
    typedef std::function<void(OperationContext* opCtx,
                               const CursorResponse& cursorResponse,
                               TxnNumber& txnNumber,
                               const ShardId& shardId,
                               const HostAndPort& donorHost)>
        WriteCallback;

    ReshardingCloneFetcher(std::shared_ptr<executor::TaskExecutor> executor,
                           std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
                           CancellationToken cancelToken,
                           std::vector<OwnedRemoteCursor> remoteCursors,
                           int numWriteThreads,
                           ReshardingMetrics* metrics)
        : _executor(std::move(executor)),
          _cleanupExecutor(std::move(cleanupExecutor)),
          _cancelSource(cancelToken),
          _factory(_cancelSource.token(), _executor),
          _remoteCursors(std::move(remoteCursors)),
          _numWriteThreads(numWriteThreads),
          _queues(_numWriteThreads),
          _activeCursors(0),
          _openConsumers(0),
          _metrics(metrics) {
        constexpr int kQueueDepthPerDonor = 2;
        MultiProducerSingleConsumerQueue<QueueData>::Options qOptions;
        qOptions.maxQueueDepth = _remoteCursors.size() * kQueueDepthPerDonor;
        for (auto& queue : _queues) {
            queue.emplace(qOptions);
        }
    }

    ~ReshardingCloneFetcher() {
        _cancelSource.cancel();
        for (auto& queue : _queues) {
            queue->closeProducerEnd();
            queue->closeConsumerEnd();
        }
        {
            stdx::unique_lock lk(_mutex);
            _allProducerConsumerClosed.wait(
                lk, [this]() { return _openConsumers == 0 && _activeCursors == 0; });
        }
    }

    void setUpWriterThreads(WriteCallback cb) {
        for (int i = 0; i < _numWriteThreads; i++) {
            {
                std::lock_guard lk(_mutex);
                _openConsumers++;
            }
            // Set up writer threads.
            auto writerFuture =
                Future<void>::makeReady()
                    .thenRunOn(_executor)
                    .then([this, cb, i] {
                        auto opCtx = _factory.makeOperationContext(&cc());
                        {
                            stdx::lock_guard lk(*opCtx->getClient());
                            opCtx->setLogicalSessionId(makeLogicalSessionId(opCtx.get()));
                        }
                        TxnNumber txnNumber(0);
                        // This loop will end by interrupt when the producer end closes.
                        while (true) {
                            auto qData = _queues[i]->pop(opCtx.get());
                            auto cursorResponse =
                                uassertStatusOK(CursorResponse::parseFromBSON(qData.data));
                            cb(opCtx.get(),
                               cursorResponse,
                               txnNumber,
                               _shardIds[qData.donorIndex],
                               qData.donorHost);
                        }
                    })
                    .thenRunOn(_cleanupExecutor)
                    .onError([this, i](Status status) {
                        LOGV2_DEBUG(7763601,
                                    2,
                                    "ReshardingCloneFetcher writer thread done",
                                    "index"_attr = i,
                                    "error"_attr = status);
                        if (!status.isOK() &&
                            status.code() != ErrorCodes::ProducerConsumerQueueConsumed) {
                            std::lock_guard lk(_mutex);
                            if (_finalResult.isOK())
                                _finalResult = status;
                            _cancelSource.cancel();
                            // If consumers fail, ensure that producers waiting on the queue
                            // exit rather than hanging.
                            _queues[i]->closeConsumerEnd();
                        }
                    })
                    .onCompletion([this](Status status) {
                        std::unique_lock lk(_mutex);
                        if (--_openConsumers == 0) {
                            _allProducerConsumerClosed.notify_all();
                        }
                    });
            _writerFutures.emplace_back(std::move(writerFuture));
        }
    }

    void handleOneResponse(const executor::TaskExecutor::ResponseStatus& response,
                           const HostAndPort& hostAndPort,
                           int index,
                           Milliseconds elapsed) {
        // To ensure that all batches from one donor are handled sequentially, we need to handle
        // those requests by only one writer thread, which is determined by the shardId, which
        // corresponds to the index here.
        int consumerIdx = index % _numWriteThreads;
        LOGV2_DEBUG(7763602,
                    3,
                    "Resharding response",
                    "index"_attr = index,
                    "shardId"_attr = _shardIds[index],
                    "consumerIndex"_attr = consumerIdx,
                    "host"_attr = hostAndPort,
                    "status"_attr = response.status,
                    "elapsed"_attr = response.elapsed,
                    "data"_attr = response.data,
                    "more"_attr = response.moreToCome);
        _metrics->onCloningRemoteBatchRetrieval(
            response.elapsed ? duration_cast<Milliseconds>(*response.elapsed) : elapsed);
        uassertStatusOK(response.status);
        reshardingCollectionClonerAbort.executeIf(
            [this](const BSONObj& data) {
                if (!_failPointHit.load()) {
                    std::lock_guard lk(_mutex);
                    // We'll fake the error and not issue any more getMores.
                    _finalResult = {ErrorCodes::SocketException, "Terminated via failpoint"};
                    _failPointHit.store(true);
                    uassertStatusOK(_finalResult);
                }
            },
            [this, index](const BSONObj& data) {
                return data["donorShard"].eoo() ||
                    data["donorShard"].valueStringDataSafe() == _shardIds[index];
            });
        _queues[consumerIdx]->push({index, hostAndPort, response.data.getOwned()});
    }

    void setupReaderThreads(OperationContext* opCtx) {
        // Network commands can start immediately, so reserve here to avoid the
        // vector being resized while setting up.
        _shardIds.reserve(_remoteCursors.size());
        for (int i = 0; i < int(_remoteCursors.size()); i++) {
            {
                std::lock_guard lk(_mutex);
                _activeCursors++;
            }
            auto& cursor = _remoteCursors[i];
            GetMoreCommandRequest getMoreRequest(
                cursor->getCursorResponse().getCursorId(),
                std::string{cursor->getCursorResponse().getNSS().coll()});
            getMoreRequest.setBatchSize(
                resharding::gReshardingCollectionClonerBatchSizeCount.load());
            BSONObj cmdObj;
            if (opCtx->getLogicalSessionId()) {
                getMoreRequest.setLsid(generic_argument_util::toLogicalSessionFromClient(
                    *opCtx->getLogicalSessionId()));
            }
            cmdObj = getMoreRequest.toBSON();

            const HostAndPort& cursorHost = cursor->getHostAndPort();
            _shardIds.push_back(ShardId(std::string{cursor->getShardId()}));
            LOGV2_DEBUG(7763603,
                        2,
                        "ReshardingCollectionCloner setting up request",
                        "index"_attr = i,
                        "shardId"_attr = _shardIds.back(),
                        "host"_attr = cursorHost);
            auto cmdFuture =
                Future<void>::makeReady()
                    .thenRunOn(_executor)
                    .then([this, i, &cursor, &cursorHost, cmdObj = std::move(cmdObj)] {
                        auto latencyTimer = std::make_shared<Timer>();
                        // TODO(SERVER-79857): This AsyncTry is being used to simulate the way the
                        // future-enabled scheduleRemoteExhaustCommand works -- the future will be
                        // fulfilled when there are no more responses forthcoming.  When we enable
                        // exhaust we can remove the AsyncTry.

                        return AsyncTry([this,
                                         &cursor,
                                         &cursorHost,
                                         i,
                                         cmdObj = cmdObj,
                                         latencyTimer]() mutable {
                                   auto opCtx = cc().makeOperationContext();
                                   executor::RemoteCommandRequest request(
                                       cursorHost,
                                       cursor->getCursorResponse().getNSS().dbName(),
                                       cmdObj,
                                       opCtx.get());
                                   latencyTimer->reset();
                                   return _executor
                                       ->scheduleRemoteCommand(request, _cancelSource.token())
                                       .then([this, &cursorHost, i, latencyTimer](
                                                 executor::TaskExecutor::ResponseStatus response) {
                                           auto duration =
                                               duration_cast<Milliseconds>(latencyTimer->elapsed());

                                           response.moreToCome = response.status.isOK() &&
                                               !response.data["cursor"].eoo() &&
                                               response.data["cursor"]["id"].safeNumberLong() != 0;
                                           handleOneResponse(response, cursorHost, i, duration);
                                           return response;
                                       });
                               })
                            .until([this](const StatusWith<executor::TaskExecutor::ResponseStatus>&
                                              swResponseStatus) {
                                return !swResponseStatus.isOK() ||
                                    !swResponseStatus.getValue().moreToCome;
                            })
                            .on(_executor, _cancelSource.token());
                    })
                    .thenRunOn(_cleanupExecutor)
                    .onCompletion(
                        [this](
                            StatusWith<executor::TaskExecutor::ResponseStatus> swResponseStatus) {
                            std::lock_guard lk(_mutex);
                            // The final result should be the first error.
                            if (_finalResult.isOK() && !swResponseStatus.isOK()) {
                                _finalResult = swResponseStatus.getStatus();
                                _cancelSource.cancel();
                            } else if (_finalResult.isOK() &&
                                       !swResponseStatus.getValue().status.isOK()) {
                                _finalResult = swResponseStatus.getValue().status;
                                _cancelSource.cancel();
                            }
                            if (--_activeCursors == 0) {
                                for (auto& queue : _queues) {
                                    queue->closeProducerEnd();
                                }
                                _allProducerConsumerClosed.notify_all();
                            }
                            return swResponseStatus;
                        });
            _cmdFutures.emplace_back(std::move(cmdFuture));
        }
    }

    ExecutorFuture<void> run(OperationContext* opCtx, WriteCallback cb) {
        setUpWriterThreads(cb);
        setupReaderThreads(opCtx);
        return whenAll(std::move(_cmdFutures))
            .thenRunOn(_executor)
            .onCompletion([this](auto ignoredStatus) {
                return whenAll(std::move(_writerFutures)).thenRunOn(_executor);
            })
            .onCompletion([this](auto ignoredStatus) { return _finalResult; });
    }

private:
    std::shared_ptr<executor::TaskExecutor> _executor;
    std::shared_ptr<executor::TaskExecutor> _cleanupExecutor;
    CancellationSource _cancelSource;
    CancelableOperationContextFactory _factory;
    std::vector<OwnedRemoteCursor> _remoteCursors;
    int _numWriteThreads;
    std::vector<ExecutorFuture<executor::TaskExecutor::ResponseStatus>> _cmdFutures;
    std::vector<ExecutorFuture<void>> _writerFutures;

    // There is one shardId per donor.
    std::vector<ShardId> _shardIds;

    struct QueueData {
        QueueData(int index, HostAndPort host, BSONObj inData)
            : donorIndex(index), donorHost(std::move(host)), data(std::move(inData)) {}
        int donorIndex;
        HostAndPort donorHost;
        BSONObj data;
    };
    std::vector<boost::optional<MultiProducerSingleConsumerQueue<QueueData>>> _queues;

    stdx::mutex _mutex;
    int _activeCursors;                  // (M)
    int _openConsumers;                  // (M)
    Status _finalResult = Status::OK();  // (M)
    AtomicWord<bool> _failPointHit;
    stdx::condition_variable _allProducerConsumerClosed;
    ReshardingMetrics* _metrics;
};

sharded_agg_helpers::DispatchShardPipelineResults
ReshardingCollectionCloner::_queryOnceWithNaturalOrder(
    OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface) {
    auto resumeData = resharding::data_copy::getRecipientResumeData(opCtx, _reshardingUUID);
    LOGV2_DEBUG(7763604,
                resumeData.empty() ? 2 : 1,
                "ReshardingCollectionCloner resume data",
                "reshardingUUID"_attr = _reshardingUUID,
                "resumeData"_attr = resumeData);

    // If running in "relaxed" mode, also instruct the receiving shards to ignore collection
    // uuid mismatches between the local and sharding catalogs.
    boost::optional<RouterRelaxCollectionUUIDConsistencyCheckBlock>
        routerRelaxCollectionUUIDConsistencyCheckBlock(boost::in_place_init_if, _relaxed, opCtx);

    sharding::router::CollectionRouter router(opCtx->getServiceContext(), _sourceNss);
    auto dispatchResults = router.routeWithRoutingContext(
        opCtx,
        "resharding collection cloner fetching with natural order (query stage)"_sd,
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            AsyncRequestsSender::ShardHostMap designatedHostsMap;
            stdx::unordered_map<ShardId, BSONObj> resumeTokenMap;
            std::set<ShardId> shardsToSkip;
            for (auto&& shardResumeData : resumeData) {
                const auto& shardId = shardResumeData.getId().getShardId();
                const auto& optionalDonorHost = shardResumeData.getDonorHost();
                const auto& optionalResumeToken = shardResumeData.getResumeToken();

                if (optionalResumeToken) {
                    // If we see a null $recordId, this means that there are no more records to read
                    // from this shard. As such, we skip it.
                    if ((*optionalResumeToken)["$recordId"].isNull()) {
                        shardsToSkip.insert(shardId);
                        continue;
                    } else {
                        resumeTokenMap[shardId] = optionalResumeToken->getOwned();
                    }
                }

                if (optionalDonorHost) {
                    designatedHostsMap[shardId] = *optionalDonorHost;
                }
            }

            auto [rawPipeline, expCtx] = makeRawNaturalOrderPipeline(opCtx, mongoProcessInterface);
            MakePipelineOptions pipelineOpts;
            pipelineOpts.attachCursorSource = false;

            // We associate the aggregation cursors established on each donor shard with a logical
            // session to prevent them from killing the cursor when it is idle locally.  While we
            // read from all cursors simultaneously, it is possible (though unlikely) for one to be
            // starved for an arbitrary period of time.
            {
                auto lk = stdx::lock_guard(*opCtx->getClient());
                opCtx->setLogicalSessionId(makeLogicalSessionId(opCtx));
            }

            auto request = AggregateCommandRequest(expCtx->getNamespaceString(), rawPipeline);
            // If running in "relaxed" mode, do not set CollectionUUID to prevent NamespaceNotFound
            // or CollectionUUIDMismatch errors.
            if (!_relaxed) {
                request.setCollectionUUID(_sourceUUID);
            }

            // In the case of a single-shard command, dispatchShardPipeline uses the passed-in batch
            // size instead of 0.  The ReshardingCloneFetcher does not handle cursors with a
            // populated first batch nor a cursor already complete (id 0), so avoid that by setting
            // the batch size to 0 here.
            SimpleCursorOptions cursorOpts;
            cursorOpts.setBatchSize(0);
            request.setCursor(cursorOpts);

            // This is intentionally not 'setRequestReshardingResumeToken'; that is used for getting
            // oplog.
            request.setRequestResumeToken(true);
            request.setHint(BSON("$natural" << 1));

            // Send with rawData since the shard key is already translated for timeseries.
            if (gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                request.setRawData(true);
            }

            auto pipeline = Pipeline::makePipeline(rawPipeline, expCtx, pipelineOpts);

            repl::ReadConcernArgs readConcern(repl::ReadConcernLevel::kSnapshotReadConcern);
            readConcern.setArgsAtClusterTimeForSnapshot(_atClusterTime);
            readConcern.setWaitLastStableRecoveryTimestamp(true);
            request.setReadConcern(readConcern);

            auto readPref = ReadPreferenceSetting{
                ReadPreference::Nearest,
                Seconds(resharding::gReshardingCollectionClonerMaxStalenessSeconds.load())};
            // The read preference on the request is merely informational (e.g. for profiler
            // entries) -- the pipeline's opCtx setting is actually used when sending the request.
            request.setUnwrappedReadPref(readPref.toContainingBSON());
            ReadPreferenceSetting::get(opCtx) = readPref;

            auto res = sharded_agg_helpers::dispatchShardPipeline(
                routingCtx,
                Document(request.toBSON()),
                sharded_agg_helpers::PipelineDataSource::kNormal,
                false /* eligibleForSampling */,
                std::move(pipeline),
                boost::none /* explain */,
                _sourceNss,
                false /* requestQueryStatsFromRemotes */,
                ShardTargetingPolicy::kAllowed,
                readConcern.toBSONInner(),
                std::move(designatedHostsMap),
                std::move(resumeTokenMap),
                std::move(shardsToSkip));

            // It is possible that all shards are included in shardsToSkip, so we safely skip
            // validation if no cursors were established.
            if (res.remoteCursors.empty()) {
                routingCtx.skipValidation();
            }
            return res;
        });

    bool hasSplitPipeline = !!dispatchResults.splitPipeline;
    std::string shardsPipelineStr;
    std::string mergePipelineStr;
    BSONObj shardCursorsSortSpec;
    if (hasSplitPipeline) {
        shardsPipelineStr =
            Value(dispatchResults.splitPipeline->shardsPipeline->serialize()).toString();
        mergePipelineStr =
            Value(dispatchResults.splitPipeline->mergePipeline->serialize()).toString();
        if (dispatchResults.splitPipeline->shardCursorsSortSpec)
            shardCursorsSortSpec = *(dispatchResults.splitPipeline->shardCursorsSortSpec);
    }
    LOGV2_DEBUG(7763600,
                2,
                "Resharding dispatch results",
                "needsSpecificShardMerger"_attr = dispatchResults.mergeShardId.has_value()
                    ? dispatchResults.mergeShardId->toString()
                    : "false",
                "numRemoteCursors"_attr = dispatchResults.remoteCursors.size(),
                "numExplainOutputs"_attr = dispatchResults.remoteExplainOutput.size(),
                "hasSplitPipeline"_attr = hasSplitPipeline,
                "shardsPipeline"_attr = shardsPipelineStr,
                "mergePipeline"_attr = mergePipelineStr,
                "shardCursorsSortSpec"_attr = shardCursorsSortSpec,
                "commandForTargetedShards"_attr = dispatchResults.commandForTargetedShards,
                "numProducers"_attr = dispatchResults.numProducers,
                "hasExchangeSpec"_attr = dispatchResults.exchangeSpec != boost::none);

    return dispatchResults;
}

void ReshardingCollectionCloner::_writeOnceWithNaturalOrder(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    std::vector<OwnedRemoteCursor> remoteCursors) {
    ReshardingCloneFetcher reshardingCloneFetcher(
        std::move(executor),
        std::move(cleanupExecutor),
        cancelToken,
        std::move(remoteCursors),
        resharding::gReshardingCollectionClonerWriteThreadCount,
        _metrics);

    if (reshardingCollectionClonerShouldFailWithStaleConfig.shouldFail()) {
        uassert(StaleConfigInfo(
                    _outputNss,
                    ShardVersionFactory::make(ChunkVersion::IGNORED()) /* receivedVersion */,
                    boost::none /* wantedVersion */,
                    ShardId{"0"}),
                str::stream() << "Throwing staleConfig for reshardingCollectionCloner failpoint.",
                false);
    }

    reshardingCloneFetcher
        .run(opCtx,
             [this](OperationContext* opCtx,
                    const CursorResponse& cursorResponse,
                    TxnNumber& txnNumber,
                    const ShardId& shardId,
                    const HostAndPort& donorHost) {
                 auto cursorBatch = cursorResponse.getBatch();
                 std::vector<InsertStatement> batch;
                 batch.reserve(cursorBatch.size());
                 for (auto&& obj : cursorBatch) {
                     batch.emplace_back(obj);
                 }
                 auto resumeToken = cursorResponse.getPostBatchResumeToken();
                 if (!resumeToken) {
                     resumeToken = BSONObj();
                 }

                 writeOneBatch(opCtx, txnNumber, batch, shardId, donorHost, *resumeToken);
             })
        .get();
}

void ReshardingCollectionCloner::_runOnceWithNaturalOrder(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken) {

    auto dispatchResults = _queryOnceWithNaturalOrder(opCtx, mongoProcessInterface);

    // If we don't establish any cursors, there is no work to do. Return.
    if (dispatchResults.remoteCursors.empty()) {
        return;
    }

    reshardingCollectionClonerPauseBeforeWriteNaturalOrder.pauseWhileSet();
    _writeOnceWithNaturalOrder(
        opCtx, executor, cleanupExecutor, cancelToken, std::move(dispatchResults.remoteCursors));
}

void ReshardingCollectionCloner::writeOneBatch(OperationContext* opCtx,
                                               TxnNumber& txnNum,
                                               std::vector<InsertStatement>& batch,
                                               ShardId donorShard,
                                               HostAndPort donorHost,
                                               BSONObj resumeToken) {
    resharding::data_copy::staleConfigShardLoop(opCtx, [&] {
        // ReshardingOpObserver depends on the collection metadata being known when processing
        // writes to the temporary resharding collection. We attach placement version IGNORED to the
        // write operations to retry on a StaleConfig error and allow the collection metadata to be
        // recovered.
        ScopedSetShardRole scopedSetShardRole(
            opCtx,
            _outputNss,
            ShardVersionFactory::make(ChunkVersion::IGNORED()) /* shardVersion */,
            boost::none /* databaseVersion */);

        Timer batchInsertTimer;
        int bytesInserted = resharding::data_copy::insertBatchTransactionally(opCtx,
                                                                              _outputNss,
                                                                              txnNum,
                                                                              batch,
                                                                              _reshardingUUID,
                                                                              donorShard,
                                                                              donorHost,
                                                                              resumeToken,
                                                                              _storeProgress);

        _metrics->onDocumentsProcessed(
            batch.size(), bytesInserted, Milliseconds(batchInsertTimer.millis()));
    });
}

SemiFuture<void> ReshardingCollectionCloner::run(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    struct ChainContext {
        std::unique_ptr<Pipeline> pipeline;
        bool moreToCome = true;
        TxnNumber batchTxnNumber = TxnNumber(0);
    };

    auto chainCtx = std::make_shared<ChainContext>();

    return resharding::WithAutomaticRetry(
               [this, chainCtx, factory, executor, cleanupExecutor, cancelToken] {
                   reshardingCollectionClonerPauseBeforeAttempt.pauseWhileSet();
                   auto opCtx = factory.makeOperationContext(&cc());
                   _runOnceWithNaturalOrder(opCtx.get(),
                                            MongoProcessInterface::create(opCtx.get()),
                                            executor,
                                            cleanupExecutor,
                                            cancelToken);
                   // If we got here, we succeeded and there is no more to come.  Otherwise
                   // _runOnceWithNaturalOrder would uassert.
                   chainCtx->moreToCome = false;
               })
        .onTransientError([this](const Status& status) {
            LOGV2(5269300,
                  "Transient error while cloning sharded collection",
                  "sourceNamespace"_attr = _sourceNss,
                  "outputNamespace"_attr = _outputNss,
                  "readTimestamp"_attr = _atClusterTime,
                  "error"_attr = redact(status));
        })
        .onUnrecoverableError([this](const Status& status) {
            LOGV2_ERROR(5352400,
                        "Operation-fatal error for resharding while cloning sharded collection",
                        "sourceNamespace"_attr = _sourceNss,
                        "outputNamespace"_attr = _outputNss,
                        "readTimestamp"_attr = _atClusterTime,
                        "error"_attr = redact(status));
        })
        .until<Status>([chainCtx, factory](const Status& status) {
            if (!status.isOK() && chainCtx->pipeline) {
                auto opCtx = factory.makeOperationContext(&cc());
                chainCtx->pipeline.reset();
            }

            return status.isOK() && !chainCtx->moreToCome;
        })
        .on(std::move(executor), cancelToken)
        .thenRunOn(std::move(cleanupExecutor))
        // It is unsafe to capture `this` once the task is running on the cleanupExecutor because
        // RecipientStateMachine, along with its ReshardingCollectionCloner member, may have already
        // been destructed.
        .onCompletion([chainCtx](Status status) {
            if (chainCtx->pipeline) {
                // TODO(SERVER-74658): Please revisit if this thread could be made killable.
                auto client = cc().getServiceContext()
                                  ->getService(ClusterRole::ShardServer)
                                  ->makeClient("ReshardingCollectionClonerCleanupClient",
                                               Client::noSession(),
                                               ClientOperationKillableByStepdown{false});

                AlternativeClientRegion acr(client);
                auto opCtx = cc().makeOperationContext();

                // Guarantee the pipeline is always cleaned up - even upon cancellation.
                chainCtx->pipeline.reset();
            }

            // Propagate the result of the AsyncTry.
            return status;
        })
        .semi();
}

}  // namespace mongo
