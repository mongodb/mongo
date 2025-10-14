/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_clone_fetcher.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"

#include <mutex>
#include <string>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

// For simulating errors while cloning. Takes "donorShard" as data.
MONGO_FAIL_POINT_DEFINE(reshardingCollectionClonerAbort);

namespace mongo {

void ReshardingCloneFetcher::setUpWriterThreads(WriteCallback cb) {
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

void ReshardingCloneFetcher::handleOneResponse(
    const executor::TaskExecutor::ResponseStatus& response,
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

void ReshardingCloneFetcher::setupReaderThreads(OperationContext* opCtx) {
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
        getMoreRequest.setBatchSize(resharding::gReshardingCollectionClonerBatchSizeCount.load());
        BSONObj cmdObj;
        if (opCtx->getLogicalSessionId()) {
            getMoreRequest.setLsid(
                generic_argument_util::toLogicalSessionFromClient(*opCtx->getLogicalSessionId()));
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
                    [this](StatusWith<executor::TaskExecutor::ResponseStatus> swResponseStatus) {
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

ExecutorFuture<void> ReshardingCloneFetcher::run(OperationContext* opCtx, WriteCallback cb) {
    setUpWriterThreads(cb);
    setupReaderThreads(opCtx);
    return whenAll(std::move(_cmdFutures))
        .thenRunOn(_executor)
        .onCompletion([this](auto ignoredStatus) {
            return whenAll(std::move(_writerFutures)).thenRunOn(_executor);
        })
        .onCompletion([this](auto ignoredStatus) { return _finalResult; });
}

}  // namespace mongo
