// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/exec/owned_remote_cursor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/producer_consumer_queue.h"

#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {

namespace executor {

class TaskExecutor;

}  // namespace executor

class OperationContext;
class ReshardingMetrics;

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
          _factory(std::make_shared<HierarchicalCancelableOperationContextFactory>(
              _cancelSource.token(), _executor)),
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
            std::unique_lock lk(_mutex);
            _allProducerConsumerClosed.wait(
                lk, [this]() { return _openConsumers == 0 && _activeCursors == 0; });
        }
    }

    void setUpWriterThreads(WriteCallback cb);

    void handleOneResponse(const executor::TaskExecutor::ResponseStatus& response,
                           const HostAndPort& hostAndPort,
                           int index,
                           Milliseconds elapsed);

    void setupReaderThreads(OperationContext* opCtx);

    ExecutorFuture<void> run(OperationContext* opCtx, WriteCallback cb);

private:
    std::shared_ptr<executor::TaskExecutor> _executor;
    std::shared_ptr<executor::TaskExecutor> _cleanupExecutor;
    CancellationSource _cancelSource;
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> _factory;
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

    std::mutex _mutex;
    int _activeCursors;                  // (M)
    int _openConsumers;                  // (M)
    Status _finalResult = Status::OK();  // (M)
    Atomic<bool> _failPointHit;
    stdx::condition_variable _allProducerConsumerClosed;
    ReshardingMetrics* _metrics;
};

}  // namespace mongo
