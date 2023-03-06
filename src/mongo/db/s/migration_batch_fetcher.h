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

#include "mongo/base/error_extra_info.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/migration_batch_inserter.h"
#include "mongo/db/s/migration_batch_mock_inserter.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/semaphore_ticketholder.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/producer_consumer_queue.h"

#pragma once

namespace mongo {


// This class is only instantiated on the destination of a chunk migration and
// has a single purpose: to manage two thread pools, one
// on which threads perform inserters, and one on which
// threads run _migrateClone requests (to fetch batches of documents to insert).
//
// The constructor creates and starts the inserter thread pool.  The destructor shuts down
// and joins the inserter thread pool.
//
// The main work of the class is in method fetchAndScheduleInsertion.  That method
// starts a thread pool for fetchers.  Each thread in that thread pool sits in a loop
// sending out _migrateClone requests, blocking on the response, and scheduling an
// inserter on the inserter thread pool.  This function joins and shuts down the
// fetcher thread pool once all batches have been fetched.
//
// Inserter is templated only to allow a mock inserter to exist.
// There is only one implementation of inserter currently, which is MigrationBatchInserter.
//
// A few things to note:
//  - After fetchAndScheduleInsertion returns, insertions are still being executed (although fetches
//    are not).
//  - Sending out _migrateClone requests in parallel implies the need for synchronization on the
//    source.  See the comments in migration_chunk_cloner_source.h for details around
//    that.
//  - The requirement on source side synchronization implies that care must be taken on upgrade.
//    In particular, if the source is running an earlier binary that doesn't have code for
//    source side synchronization, it is unsafe to send _migrateClone requests in parallel.
//    To handle that case, when the source is prepared to service _migrateClone requests in
//    parallel, the field "parallelMigrateCloneSupported" is included in the "_recvChunkStart"
//    command.  The inclusion of that field indicates to the destination that it is safe
//    to send _migrateClone requests in parallel.  Its exclusion indicates that it is unsafe.
template <typename Inserter>
class MigrationBatchFetcher {
public:
    MigrationBatchFetcher(OperationContext* outerOpCtx,
                          OperationContext* innerOpCtx,
                          NamespaceString nss,
                          MigrationSessionId sessionId,
                          const WriteConcernOptions& writeConcern,
                          const ShardId& fromShardId,
                          const ChunkRange& range,
                          const UUID& migrationId,
                          const UUID& collectionId,
                          std::shared_ptr<MigrationCloningProgressSharedState> migrationInfo,
                          bool parallelFetchingSupported);

    ~MigrationBatchFetcher();

    // Repeatedly fetch batches (using _migrateClone request) and schedule inserter jobs
    // on thread pool.
    void fetchAndScheduleInsertion();

    // Get inserter thread pool stats.
    ThreadPool::Stats getThreadPoolStats() const {
        return _inserterWorkers->getStats();
    }

private:
    NamespaceString _nss;

    // Size of thread pools.
    int _chunkMigrationConcurrency;

    MigrationSessionId _sessionId;

    // Inserter thread pool.
    std::unique_ptr<ThreadPool> _inserterWorkers;

    BSONObj _migrateCloneRequest;

    OperationContext* _outerOpCtx;

    OperationContext* _innerOpCtx;

    std::shared_ptr<Shard> _fromShard;

    // Shared state, by which the progress of migration is communicated
    // to MigrationDestinationManager.
    std::shared_ptr<MigrationCloningProgressSharedState> _migrationProgress;

    ChunkRange _range;

    UUID _collectionUuid;

    UUID _migrationId;

    WriteConcernOptions _writeConcern;

    // Indicates if source is prepared to service _migrateClone requests in parallel.
    bool _isParallelFetchingSupported;

    SemaphoreTicketHolder _secondaryThrottleTicket;

    // Given session id and namespace, create migrateCloneRequest.
    // Only should be created once for the lifetime of the object.
    BSONObj _createMigrateCloneRequest() const {
        BSONObjBuilder builder;
        builder.append("_migrateClone", _nss.ns());
        _sessionId.append(&builder);
        return builder.obj();
    }

    void _runFetcher();

    // Fetches next batch using _migrateClone request and return it.  May return an empty batch.
    BSONObj _fetchBatch(OperationContext* opCtx);

    static bool _isEmptyBatch(const BSONObj& batch) {
        return batch.getField("objects").Obj().isEmpty();
    }

    static void onCreateThread(const std::string& threadName) {
        Client::initThread(threadName, getGlobalServiceContext(), nullptr);
        {
            stdx::lock_guard<Client> lk(cc());
            cc().setSystemOperationKillableByStepdown(lk);
        }
    }

};  // namespace mongo

}  // namespace mongo
