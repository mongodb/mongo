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

#include "mongo/db/s/migration_batch_fetcher.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

template <typename Inserter>
MigrationBatchFetcher<Inserter>::MigrationBatchFetcher(
    OperationContext* outerOpCtx,
    OperationContext* innerOpCtx,
    NamespaceString nss,
    MigrationSessionId sessionId,
    const WriteConcernOptions& writeConcern,
    const ShardId& fromShardId,
    const ChunkRange& range,
    const UUID& migrationId,
    const UUID& collectionId,
    std::shared_ptr<MigrationCloningProgressSharedState> migrationProgress,
    bool parallelFetchingSupported)
    : _nss{std::move(nss)},
      _chunkMigrationConcurrency{
          // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
          mongo::feature_flags::gConcurrencyInChunkMigration.isEnabledAndIgnoreFCVUnsafe()
              ? chunkMigrationConcurrency.load()
              : 1},
      _sessionId{std::move(sessionId)},
      _inserterWorkers{[&]() {
          ThreadPool::Options options;
          options.poolName = "ChunkMigrationInserters";
          options.minThreads = _chunkMigrationConcurrency;
          options.maxThreads = _chunkMigrationConcurrency;
          options.onCreateThread = Inserter::onCreateThread;
          return std::make_unique<ThreadPool>(options);
      }()},
      _migrateCloneRequest{_createMigrateCloneRequest()},
      _outerOpCtx{outerOpCtx},
      _innerOpCtx{innerOpCtx},
      _fromShard{uassertStatusOK(
          Grid::get(_outerOpCtx)->shardRegistry()->getShard(_outerOpCtx, fromShardId))},
      _migrationProgress{migrationProgress},
      _range{range},
      _collectionUuid(collectionId),
      _migrationId{migrationId},
      _writeConcern{writeConcern},
      _isParallelFetchingSupported{parallelFetchingSupported},
      _secondaryThrottleTicket(1, outerOpCtx->getServiceContext()) {
    _inserterWorkers->startup();
}

template <typename Inserter>
BSONObj MigrationBatchFetcher<Inserter>::_fetchBatch(OperationContext* opCtx) {
    auto commandResponse = uassertStatusOKWithContext(
        _fromShard->runCommand(opCtx,
                               ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                               "admin",
                               _migrateCloneRequest,
                               Shard::RetryPolicy::kNoRetry),
        "_migrateClone failed: ");

    uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(commandResponse),
                               "_migrateClone failed: ");

    return commandResponse.response;
}

template <typename Inserter>
void MigrationBatchFetcher<Inserter>::fetchAndScheduleInsertion() {
    auto numFetchers = _isParallelFetchingSupported ? _chunkMigrationConcurrency : 1;
    auto fetchersThreadPool = [&]() {
        ThreadPool::Options options;
        options.poolName = "ChunkMigrationFetchers";
        options.minThreads = numFetchers;
        options.maxThreads = numFetchers;
        options.onCreateThread = onCreateThread;
        return std::make_unique<ThreadPool>(options);
    }();
    fetchersThreadPool->startup();
    for (int i = 0; i < numFetchers; ++i) {
        fetchersThreadPool->schedule([this](Status status) { this->_runFetcher(); });
    }

    fetchersThreadPool->shutdown();
    fetchersThreadPool->join();
}


template <typename Inserter>
void MigrationBatchFetcher<Inserter>::_runFetcher() try {
    auto executor =
        Grid::get(_innerOpCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();

    auto applicationOpCtx = CancelableOperationContext(
        cc().makeOperationContext(), _innerOpCtx->getCancellationToken(), executor);

    auto opCtx = applicationOpCtx.get();
    auto assertNotAborted = [&]() {
        {
            stdx::lock_guard<Client> lk(*_outerOpCtx->getClient());
            _outerOpCtx->checkForInterrupt();
        }
        opCtx->checkForInterrupt();
    };

    LOGV2_DEBUG(6718405, 0, "Chunk migration data fetch start", "migrationId"_attr = _migrationId);
    while (true) {
        Timer totalTimer;
        BSONObj nextBatch = _fetchBatch(opCtx);
        assertNotAborted();
        if (_isEmptyBatch(nextBatch)) {
            LOGV2_DEBUG(6718404,
                        0,
                        "Chunk migration initial clone complete",
                        "migrationId"_attr = _migrationId,
                        "duration"_attr = totalTimer.elapsed());
            break;
        }

        const auto batchSize = nextBatch.objsize();
        const auto fetchTime = totalTimer.elapsed();
        LOGV2_DEBUG(6718416,
                    0,
                    "Chunk migration initial clone fetch end",
                    "migrationId"_attr = _migrationId,
                    "batchSize"_attr = batchSize,
                    "fetch"_attr = duration_cast<Milliseconds>(fetchTime));


        Inserter inserter{_outerOpCtx,
                          _innerOpCtx,
                          nextBatch.getOwned(),
                          _nss,
                          _range,
                          _writeConcern,
                          _collectionUuid,
                          _migrationProgress,
                          _migrationId,
                          _chunkMigrationConcurrency,
                          &_secondaryThrottleTicket};

        _inserterWorkers->schedule([batchSize,
                                    fetchTime,
                                    totalTimer = std::move(totalTimer),
                                    insertTimer = Timer(),
                                    migrationId = _migrationId,
                                    inserter = std::move(inserter)](Status status) {
            inserter.run(status);

            const auto checkDivByZero = [](auto divisor, auto expression) {
                return divisor == 0 ? -1 : expression();
            };
            const auto calcThroughput = [&](auto bytes, auto duration) {
                return checkDivByZero(durationCount<Microseconds>(duration), [&]() {
                    return static_cast<double>(bytes) / durationCount<Microseconds>(duration);
                });
            };

            const auto insertTime = insertTimer.elapsed();
            const auto totalTime = totalTimer.elapsed();
            const auto batchThroughputMBps = calcThroughput(batchSize, totalTime);
            const auto insertThroughputMBps = calcThroughput(batchSize, insertTime);
            const auto fetchThroughputMBps = calcThroughput(batchSize, fetchTime);

            LOGV2_DEBUG(6718417,
                        1,
                        "Chunk migration initial clone apply batch",
                        "migrationId"_attr = migrationId,
                        "batchSize"_attr = batchSize,
                        "total"_attr = duration_cast<Milliseconds>(totalTime),
                        "totalThroughputMBps"_attr = batchThroughputMBps,
                        "fetch"_attr = duration_cast<Milliseconds>(fetchTime),
                        "fetchThroughputMBps"_attr = fetchThroughputMBps,
                        "insert"_attr = duration_cast<Milliseconds>(insertTime),
                        "insertThroughputMBps"_attr = insertThroughputMBps);
        });
    }
} catch (const DBException& e) {
    stdx::lock_guard<Client> lk(*_innerOpCtx->getClient());
    _innerOpCtx->getServiceContext()->killOperation(lk, _innerOpCtx, ErrorCodes::Error(6718400));
    LOGV2_ERROR(6718413,
                "Chunk migration failure fetching data",
                "migrationId"_attr = _migrationId,
                "failure"_attr = e.toStatus());
}

template <typename Inserter>
MigrationBatchFetcher<Inserter>::~MigrationBatchFetcher() {
    LOGV2(6718401,
          "Shutting down and joining inserter threads for migration {migrationId}",
          "migrationId"_attr = _migrationId);

    // Call waitForIdle first since join can spawn another thread while ignoring the maxPoolSize
    // to finish the pending task. This is safe as long as ThreadPool::shutdown can't be
    // interleaved with this call.
    _inserterWorkers->waitForIdle();
    _inserterWorkers->shutdown();
    _inserterWorkers->join();

    LOGV2(6718415,
          "Inserter threads for migration {migrationId} joined",
          "migrationId"_attr = _migrationId);
}

template class MigrationBatchFetcher<MigrationBatchInserter>;

template class MigrationBatchFetcher<MigrationBatchMockInserter>;

}  // namespace mongo
