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


#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_index_catalog_cache.h"
#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

// For simulating errors while cloning. Takes "donorShard" as data.
MONGO_FAIL_POINT_DEFINE(reshardingCollectionClonerAbort);

MONGO_FAIL_POINT_DEFINE(reshardingCollectionClonerPauseBeforeAttempt);

namespace mongo {
namespace {

bool collectionHasSimpleCollation(OperationContext* opCtx, const NamespaceString& nss) {
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    auto [sourceChunkMgr, _] = catalogCache->getTrackedCollectionRoutingInfo(opCtx, nss);

    return !sourceChunkMgr.getDefaultCollator();
}

}  // namespace

ReshardingCollectionCloner::ReshardingCollectionCloner(ReshardingMetrics* metrics,
                                                       const UUID& reshardingUUID,
                                                       ShardKeyPattern newShardKeyPattern,
                                                       NamespaceString sourceNss,
                                                       const UUID& sourceUUID,
                                                       ShardId recipientShard,
                                                       Timestamp atClusterTime,
                                                       NamespaceString outputNss)
    : _metrics(metrics),
      _reshardingUUID(reshardingUUID),
      _newShardKeyPattern(std::move(newShardKeyPattern)),
      _sourceNss(std::move(sourceNss)),
      _sourceUUID(sourceUUID),
      _recipientShard(std::move(recipientShard)),
      _atClusterTime(atClusterTime),
      _outputNss(std::move(outputNss)) {}

std::pair<std::vector<BSONObj>, boost::intrusive_ptr<ExpressionContext>>
ReshardingCollectionCloner::makeRawPipeline(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    Value resumeId) {
    // Assume that the input collection isn't a view. The collectionUUID parameter to
    // the aggregate would enforce this anyway.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[_sourceNss.coll()] = {_sourceNss, std::vector<BSONObj>{}};

    // Assume that the config.cache.chunks collection isn't a view either.
    auto tempNss =
        resharding::constructTemporaryReshardingNss(_sourceNss.db_forSharding(), _sourceUUID);
    auto tempCacheChunksNss = NamespaceString::makeGlobalConfigCollection(
        "cache.chunks." +
        NamespaceStringUtil::serialize(tempNss, SerializationContext::stateDefault()));
    resolvedNamespaces[tempCacheChunksNss.coll()] = {tempCacheChunksNss, std::vector<BSONObj>{}};

    // Pipeline::makePipeline() ignores the collation set on the AggregationRequest (or lack
    // thereof) and instead only considers the collator set on the ExpressionContext. Setting
    // nullptr as the collator on the ExpressionContext means that the aggregation pipeline is
    // always using the "simple" collation, even when the collection default collation for
    // _sourceNss is non-simple. The chunk ranges in the $lookup stage must be compared using the
    // simple collation because collections are always sharded using the simple collation. However,
    // resuming by _id is only efficient (i.e. non-blocking seek/sort) when the aggregation pipeline
    // would be using the collection's default collation. We cannot do both so we choose to disallow
    // automatic resuming for collections with non-simple default collations.
    uassert(4929303,
            "Cannot resume cloning when sharded collection has non-simple default collation",
            resumeId.missing() || collectionHasSimpleCollation(opCtx, _sourceNss));

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    boost::none, /* explain */
                                                    false,       /* fromMongos */
                                                    false,       /* needsMerge */
                                                    false,       /* allowDiskUse */
                                                    false,       /* bypassDocumentValidation */
                                                    false,       /* isMapReduceCommand */
                                                    _sourceNss,
                                                    boost::none, /* runtimeConstants */
                                                    nullptr,     /* collator */
                                                    std::move(mongoProcessInterface),
                                                    std::move(resolvedNamespaces),
                                                    _sourceUUID);

    std::vector<BSONObj> rawPipeline;

    if (!resumeId.missing()) {
        rawPipeline.emplace_back(BSON(
            "$match" << BSON(
                "$expr" << BSON("$gte" << BSON_ARRAY("$_id" << BSON("$literal" << resumeId))))));
    }

    auto keyPattern = ShardKeyPattern(_newShardKeyPattern.getKeyPattern()).toBSON();
    rawPipeline.emplace_back(
        BSON(DocumentSourceReshardingOwnershipMatch::kStageName
             << BSON("recipientShardId" << _recipientShard << "reshardingKey" << keyPattern)));

    // We use $arrayToObject to synthesize the $sortKeys needed by the AsyncResultsMerger to
    // merge the results from all of the donor shards by {_id: 1}. This expression wouldn't
    // be correct if the aggregation pipeline was using a non-"simple" collation.
    rawPipeline.emplace_back(
        fromjson("{$replaceWith: {$mergeObjects: [\
            '$$ROOT',\
            {$arrayToObject: {$concatArrays: [[{\
                k: {$literal: '$sortKey'},\
                v: ['$$ROOT._id']\
            }]]}}\
        ]}}"));

    return std::make_pair(std::move(rawPipeline), std::move(expCtx));
}

std::pair<std::vector<BSONObj>, boost::intrusive_ptr<ExpressionContext>>
ReshardingCollectionCloner::makeRawNaturalOrderPipeline(
    OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface) {
    // Assume that the input collection isn't a view. The collectionUUID parameter to
    // the aggregate would enforce this anyway.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[_sourceNss.coll()] = {_sourceNss, std::vector<BSONObj>{}};

    // Assume that the config.cache.chunks collection isn't a view either.
    auto tempNss =
        resharding::constructTemporaryReshardingNss(_sourceNss.db_forSharding(), _sourceUUID);
    auto tempCacheChunksNss = NamespaceString::makeGlobalConfigCollection(
        "cache.chunks." +
        NamespaceStringUtil::serialize(tempNss, SerializationContext::stateDefault()));
    resolvedNamespaces[tempCacheChunksNss.coll()] = {tempCacheChunksNss, std::vector<BSONObj>{}};

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    boost::none, /* explain */
                                                    false,       /* fromMongos */
                                                    false,       /* needsMerge */
                                                    false,       /* allowDiskUse */
                                                    false,       /* bypassDocumentValidation */
                                                    false,       /* isMapReduceCommand */
                                                    _sourceNss,
                                                    boost::none, /* runtimeConstants */
                                                    nullptr,     /* collator */
                                                    std::move(mongoProcessInterface),
                                                    std::move(resolvedNamespaces),
                                                    _sourceUUID);

    std::vector<BSONObj> rawPipeline;

    auto keyPattern = ShardKeyPattern(_newShardKeyPattern.getKeyPattern()).toBSON();
    rawPipeline.emplace_back(
        BSON(DocumentSourceReshardingOwnershipMatch::kStageName
             << BSON("recipientShardId" << _recipientShard << "reshardingKey" << keyPattern)));

    return std::make_pair(std::move(rawPipeline), std::move(expCtx));
}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingCollectionCloner::_targetAggregationRequest(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto opCtx = expCtx->opCtx;
    // We associate the aggregation cursors established on each donor shard with a logical
    // session to prevent them from killing the cursor when it is idle locally. Due to the
    // cursor's merging behavior across all donor shards, it is possible for the cursor to be
    // active on one donor shard while idle for a long period on another donor shard.
    {
        auto lk = stdx::lock_guard(*opCtx->getClient());
        opCtx->setLogicalSessionId(makeLogicalSessionId(opCtx));
    }

    AggregateCommandRequest request(_sourceNss, rawPipeline);
    request.setCollectionUUID(_sourceUUID);

    auto hint = collectionHasSimpleCollation(opCtx, _sourceNss)
        ? boost::optional<BSONObj>{BSON("_id" << 1)}
        : boost::none;

    if (hint) {
        request.setHint(*hint);
    }

    request.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kSnapshotName
                                << repl::ReadConcernArgs::kAtClusterTimeFieldName
                                << _atClusterTime));

    // The read preference on the request is merely informational (e.g. for profiler entries) -- the
    // pipeline's opCtx setting is actually used when sending the request.
    auto readPref = ReadPreferenceSetting{ReadPreference::Nearest};
    request.setUnwrappedReadPref(readPref.toContainingBSON());
    ReadPreferenceSetting::get(opCtx) = readPref;

    return shardVersionRetry(opCtx,
                             Grid::get(opCtx)->catalogCache(),
                             _sourceNss,
                             "targeting donor shards for resharding collection cloning"_sd,
                             [&] {
                                 // We use the hint as an implied sort for $mergeCursors because
                                 // the aggregation pipeline synthesizes the necessary $sortKeys
                                 // fields in the result set.
                                 return Pipeline::makePipeline(request, expCtx, hint);
                             });
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
                           sharded_agg_helpers::DispatchShardPipelineResults dispatchResults,
                           int batchSizeLimitBytes,
                           int numWriteThreads)
        : _executor(std::move(executor)),
          _cleanupExecutor(std::move(cleanupExecutor)),
          _cancelSource(cancelToken),
          _factory(_cancelSource.token(), _executor),
          _dispatchResults(std::move(dispatchResults)),
          _numWriteThreads(numWriteThreads),
          _queues(_numWriteThreads),
          _activeCursors(0),
          _openConsumers(0) {
        constexpr int kQueueDepthPerDonor = 2;
        MultiProducerSingleConsumerQueue<QueueData>::Options qOptions;
        qOptions.maxQueueDepth = _dispatchResults.remoteCursors.size() * kQueueDepthPerDonor;
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
                           int index) {
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
        auto& remoteCursors = _dispatchResults.remoteCursors;
        // Network commands can start immediately, so reserve here to avoid the
        // vector being resized while setting up.
        _shardIds.reserve(remoteCursors.size());
        for (int i = 0; i < int(remoteCursors.size()); i++) {
            {
                std::lock_guard lk(_mutex);
                _activeCursors++;
            }
            auto& cursor = _dispatchResults.remoteCursors[i];
            GetMoreCommandRequest getMoreRequest(
                cursor->getCursorResponse().getCursorId(),
                cursor->getCursorResponse().getNSS().coll().toString());
            BSONObj cmdObj;
            if (opCtx->getLogicalSessionId()) {
                BSONObjBuilder cmdObjWithLsidBuilder;
                BSONObjBuilder lsidBuilder(cmdObjWithLsidBuilder.subobjStart(
                    OperationSessionInfoFromClient::kSessionIdFieldName));
                opCtx->getLogicalSessionId()->serialize(&lsidBuilder);
                lsidBuilder.doneFast();
                cmdObj = getMoreRequest.toBSON(cmdObjWithLsidBuilder.done());
            } else {
                cmdObj = getMoreRequest.toBSON({});
            }

            const HostAndPort& cursorHost = cursor->getHostAndPort();
            _shardIds.push_back(ShardId(cursor->getShardId().toString()));
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
                        // TODO(SERVER-79857): This AsyncTry is being used to simulate the way the
                        // future-enabled scheduleRemoteExhaustCommand works -- the future will be
                        // fulfilled when there are no more responses forthcoming.  When we enable
                        // exhaust we can remove the AsyncTry.
                        return AsyncTry([this, &cursor, &cursorHost, i, cmdObj = cmdObj] {
                                   auto opCtx = cc().makeOperationContext();
                                   executor::RemoteCommandRequest request(
                                       cursorHost,
                                       cursor->getCursorResponse().getNSS().dbName(),
                                       cmdObj,
                                       opCtx.get());
                                   return _executor
                                       ->scheduleRemoteCommand(request, _cancelSource.token())
                                       .then([this, &cursorHost, i](
                                                 executor::TaskExecutor::ResponseStatus response) {
                                           response.moreToCome = response.status.isOK() &&
                                               !response.data["cursor"].eoo() &&
                                               response.data["cursor"]["id"].safeNumberLong() != 0;
                                           handleOneResponse(response, cursorHost, i);
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
    sharded_agg_helpers::DispatchShardPipelineResults _dispatchResults;
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

    Mutex _mutex = MONGO_MAKE_LATCH("ReshardingCloneFetcher::_mutex");
    int _activeCursors;                  // (M)
    int _openConsumers;                  // (M)
    Status _finalResult = Status::OK();  // (M)
    AtomicWord<bool> _failPointHit;
    stdx::condition_variable _allProducerConsumerClosed;
};

void ReshardingCollectionCloner::_runOnceWithNaturalOrder(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken) {
    auto resumeData = resharding::data_copy::getRecipientResumeData(opCtx, _reshardingUUID);
    LOGV2_DEBUG(7763604,
                resumeData.empty() ? 2 : 1,
                "ReshardingCollectionCloner resume data",
                "reshardingUUID"_attr = _reshardingUUID,
                "resumeData"_attr = resumeData);
    AsyncRequestsSender::ShardHostMap designatedHostsMap;
    stdx::unordered_map<ShardId, BSONObj> resumeTokenMap;
    std::set<ShardId> shardsToSkip;
    for (auto&& shardResumeData : resumeData) {
        const auto& shardId = shardResumeData.getId().getShardId();
        const auto& optionalDonorHost = shardResumeData.getDonorHost();
        const auto& optionalResumeToken = shardResumeData.getResumeToken();

        if (optionalResumeToken) {
            // If we see a null $recordId, this means that there are no more records to read from
            // this shard. As such, we skip it.
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
    // read from all cursors simultaneously, it is possible (though unlikely) for one to be starved
    // for an arbitrary period of time.
    {
        auto lk = stdx::lock_guard(*opCtx->getClient());
        opCtx->setLogicalSessionId(makeLogicalSessionId(opCtx));
    }

    auto request = AggregateCommandRequest(expCtx->ns, rawPipeline);
    request.setCollectionUUID(_sourceUUID);
    // In the case of a single-shard command, dispatchShardPipeline uses the passed-in batch
    // size instead of 0.  The ReshardingCloneFetcher does not handle cursors with a populated
    // first batch nor a cursor already complete (id 0), so avoid that by setting the batch size
    // to 0 here.
    SimpleCursorOptions cursorOpts;
    cursorOpts.setBatchSize(0);
    request.setCursor(cursorOpts);

    // This is intentionally not 'setRequestReshardingResumeToken'; that is used for getting
    // oplog.
    request.setRequestResumeToken(true);
    request.setHint(BSON("$natural" << 1));

    auto pipeline = Pipeline::makePipeline(rawPipeline, expCtx, pipelineOpts);

    const Document serializedCommand = aggregation_request_helper::serializeToCommandDoc(request);
    auto readConcern = BSON(repl::ReadConcernArgs::kLevelFieldName
                            << repl::readConcernLevels::kSnapshotName
                            << repl::ReadConcernArgs::kAtClusterTimeFieldName << _atClusterTime
                            << repl::ReadConcernArgs::kWaitLastStableRecoveryTimestamp << true);
    request.setReadConcern(readConcern);

    // The read preference on the request is merely informational (e.g. for profiler entries) -- the
    // pipeline's opCtx setting is actually used when sending the request.
    auto readPref = ReadPreferenceSetting{ReadPreference::Nearest};
    request.setUnwrappedReadPref(readPref.toContainingBSON());
    ReadPreferenceSetting::get(opCtx) = readPref;

    auto dispatchResults =
        sharded_agg_helpers::dispatchShardPipeline(serializedCommand,
                                                   sharded_agg_helpers::PipelineDataSource::kNormal,
                                                   false /* eligibleForSampling */,
                                                   std::move(pipeline),
                                                   boost::none /* explain */,
                                                   boost::none /* cri */,
                                                   ShardTargetingPolicy::kAllowed,
                                                   readConcern,
                                                   std::move(designatedHostsMap),
                                                   std::move(resumeTokenMap),
                                                   std::move(shardsToSkip));

    // If we don't establish any cursors, there is no work to do. Return.
    if (dispatchResults.remoteCursors.empty()) {
        return;
    }

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

    ReshardingCloneFetcher reshardingCloneFetcher(
        std::move(executor),
        std::move(cleanupExecutor),
        cancelToken,
        std::move(dispatchResults),
        resharding::gReshardingCollectionClonerBatchSizeInBytes.load(),
        resharding::gReshardingCollectionClonerWriteThreadCount);
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

                 writeOneBatch(opCtx,
                               txnNumber,
                               batch,
                               shardId,
                               donorHost,
                               *resumeToken,
                               true /*useNaturalOrderCloner*/);
             })
        .get();
}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingCollectionCloner::_restartPipeline(
    OperationContext* opCtx, std::shared_ptr<executor::TaskExecutor> executor) {
    auto idToResumeFrom = [&] {
        AutoGetCollection outputColl(opCtx, _outputNss, MODE_IS);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Resharding collection cloner's output collection '"
                              << _outputNss.toStringForErrorMsg() << "' did not already exist",
                outputColl);
        return resharding::data_copy::findHighestInsertedId(opCtx, *outputColl);
    }();

    // The BlockingResultsMerger underlying by the $mergeCursors stage records how long the
    // recipient spent waiting for documents from the donor shards. It doing so requires the CurOp
    // to be marked as having started.
    auto* curOp = CurOp::get(opCtx);
    curOp->ensureStarted();
    ON_BLOCK_EXIT([curOp] { curOp->done(); });

    auto [rawPipeline, expCtx] =
        makeRawPipeline(opCtx, MongoProcessInterface::create(opCtx), idToResumeFrom);

    auto pipeline = _targetAggregationRequest(rawPipeline, expCtx);

    if (!idToResumeFrom.missing()) {
        // Skip inserting the first document retrieved after resuming because $gte was used in the
        // aggregation pipeline.
        auto firstDoc = pipeline->getNext();
        uassert(4929301,
                str::stream() << "Expected pipeline to retrieve document with _id: "
                              << redact(idToResumeFrom.toString()),
                firstDoc);

        // Note that the following uassert() could throw because we're using the simple string
        // comparator and the collection could have a non-simple collation. However, it would still
        // be correct to throw an exception because it would mean the collection being resharded
        // contains multiple documents with the same _id value as far as global uniqueness is
        // concerned.
        const auto& firstId = (*firstDoc)["_id"];
        uassert(4929302,
                str::stream() << "Expected pipeline to retrieve document with _id: "
                              << redact(idToResumeFrom.toString())
                              << ", but got _id: " << redact(firstId.toString()),
                ValueComparator::kInstance.evaluate(firstId == idToResumeFrom));
    }

    pipeline->detachFromOperationContext();
    pipeline.get_deleter().dismissDisposal();
    return pipeline;
}

bool ReshardingCollectionCloner::doOneBatch(OperationContext* opCtx,
                                            Pipeline& pipeline,
                                            TxnNumber& txnNum) {
    pipeline.reattachToOperationContext(opCtx);
    ON_BLOCK_EXIT([&pipeline] { pipeline.detachFromOperationContext(); });

    Timer latencyTimer;
    auto batch = resharding::data_copy::fillBatchForInsert(
        pipeline, resharding::gReshardingCollectionClonerBatchSizeInBytes.load());

    _metrics->onCloningRemoteBatchRetrieval(duration_cast<Milliseconds>(latencyTimer.elapsed()));

    if (batch.empty()) {
        return false;
    }

    writeOneBatch(opCtx, txnNum, batch);
    return true;
}

void ReshardingCollectionCloner::writeOneBatch(OperationContext* opCtx,
                                               TxnNumber& txnNum,
                                               std::vector<InsertStatement>& batch,
                                               ShardId donorShard,
                                               HostAndPort donorHost,
                                               BSONObj resumeToken,
                                               bool useNaturalOrderCloner) {
    Timer batchInsertTimer;
    int bytesInserted = resharding::data_copy::withOneStaleConfigRetry(opCtx, [&] {
        // ReshardingOpObserver depends on the collection metadata being known when processing
        // writes to the temporary resharding collection. We attach shard version IGNORED to the
        // insert operations and retry once on a StaleConfig error to allow the collection metadata
        // information to be recovered.
        auto [_, sii] =
            Grid::get(opCtx)->catalogCache()->getTrackedCollectionRoutingInfo(opCtx, _outputNss);
        if (useNaturalOrderCloner) {
            return resharding::data_copy::insertBatchTransactionally(opCtx,
                                                                     _outputNss,
                                                                     sii,
                                                                     txnNum,
                                                                     batch,
                                                                     _reshardingUUID,
                                                                     donorShard,
                                                                     donorHost,
                                                                     resumeToken);
        } else {
            ScopedSetShardRole scopedSetShardRole(
                opCtx,
                _outputNss,
                ShardVersionFactory::make(ChunkVersion::IGNORED(),
                                          sii ? boost::make_optional(sii->getCollectionIndexes())
                                              : boost::none) /* shardVersion */,
                boost::none /* databaseVersion */);
            return resharding::data_copy::insertBatch(opCtx, _outputNss, batch);
        }
    });

    _metrics->onDocumentsProcessed(
        batch.size(), bytesInserted, Milliseconds(batchInsertTimer.millis()));
}

SemiFuture<void> ReshardingCollectionCloner::run(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    struct ChainContext {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
        bool moreToCome = true;
        TxnNumber batchTxnNumber = TxnNumber(0);
    };

    auto chainCtx = std::make_shared<ChainContext>();
    auto reshardingImprovementsEnabled = resharding::gFeatureFlagReshardingImprovements.isEnabled(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    return resharding::WithAutomaticRetry([this,
                                           chainCtx,
                                           factory,
                                           executor,
                                           cleanupExecutor,
                                           cancelToken,
                                           reshardingImprovementsEnabled] {
               reshardingCollectionClonerPauseBeforeAttempt.pauseWhileSet();
               if (reshardingImprovementsEnabled) {
                   auto opCtx = factory.makeOperationContext(&cc());
                   // We can run into StaleConfig errors when cloning collections. To make it
                   // safer during retry, we retry the whole cloning process and rely on the
                   // resume token to be correct.
                   resharding::data_copy::withOneStaleConfigRetry(opCtx.get(), [&] {
                       _runOnceWithNaturalOrder(opCtx.get(),
                                                MongoProcessInterface::create(opCtx.get()),
                                                executor,
                                                cleanupExecutor,
                                                cancelToken);
                   });
                   // If we got here, we succeeded and there is no more to come.  Otherwise
                   // _runOnceWithNaturalOrder would uassert.
                   chainCtx->moreToCome = false;
                   return;
               }
               if (!chainCtx->pipeline) {
                   auto opCtx = factory.makeOperationContext(&cc());
                   chainCtx->pipeline = _restartPipeline(opCtx.get(), executor);
               }

               auto opCtx = factory.makeOperationContext(&cc());
               ScopeGuard guard([&] {
                   chainCtx->pipeline->dispose(opCtx.get());
                   chainCtx->pipeline.reset();
               });
               chainCtx->moreToCome =
                   doOneBatch(opCtx.get(), *chainCtx->pipeline, chainCtx->batchTxnNumber);
               guard.dismiss();
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
                chainCtx->pipeline->dispose(opCtx.get());
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
                auto client = cc().getServiceContext()
                                  ->getService(ClusterRole::ShardServer)
                                  ->makeClient("ReshardingCollectionClonerCleanupClient");

                // TODO(SERVER-74658): Please revisit if this thread could be made killable.
                {
                    stdx::lock_guard<Client> lk(*client.get());
                    client.get()->setSystemOperationUnkillableByStepdown(lk);
                }

                AlternativeClientRegion acr(client);
                auto opCtx = cc().makeOperationContext();

                // Guarantee the pipeline is always cleaned up - even upon cancellation.
                chainCtx->pipeline->dispose(opCtx.get());
                chainCtx->pipeline.reset();
            }

            // Propagate the result of the AsyncTry.
            return status;
        })
        .semi();
}

}  // namespace mongo
