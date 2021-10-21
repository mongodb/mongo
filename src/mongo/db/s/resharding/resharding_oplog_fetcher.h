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
#pragma once

#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/background.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {

class ReshardingMetrics;

class ReshardingOplogFetcher : public resharding::OnInsertAwaitable {
public:
    class Env {
    public:
        Env(ServiceContext* service, ReshardingMetrics* metrics)
            : _service(service), _metrics(metrics) {}
        ServiceContext* service() const {
            return _service;
        }
        ReshardingMetrics* metrics() const {
            return _metrics;
        }

    private:
        ServiceContext* _service;
        ReshardingMetrics* _metrics;
    };

    // Special value to use for startAt to indicate there are no more oplog entries needing to be
    // fetched.
    static const ReshardingDonorOplogId kFinalOpAlreadyFetched;

    ReshardingOplogFetcher(std::unique_ptr<Env> env,
                           UUID reshardingUUID,
                           UUID collUUID,
                           ReshardingDonorOplogId startAt,
                           ShardId donorShard,
                           ShardId recipientShard,
                           NamespaceString toWriteInto);

    ~ReshardingOplogFetcher();

    Future<void> awaitInsert(const ReshardingDonorOplogId& lastSeen) override;

    /**
     * Schedules a task that will do the following:
     *
     * - Find a valid connection to fetch oplog entries from.
     * - Send an aggregation request + getMores until either:
     * -- The "final resharding" oplog entry is found.
     * -- An interruption occurs.
     * -- The fetcher concludes it's fallen off the oplog.
     * -- A different error occurs.
     *
     * In the first two circumstances, the task will terminate. If the fetcher has fallen off the
     * oplog, this is thrown as a fatal resharding exception.  In the last circumstance, the task
     * will be rescheduled in a way that resumes where it had left off from.
     */
    ExecutorFuture<void> schedule(std::shared_ptr<executor::TaskExecutor> executor,
                                  const CancellationToken& cancelToken,
                                  CancelableOperationContextFactory factory);

    /**
     * Given a shard, fetches and copies oplog entries until
     *  - reaching an error,
     *  - coming across a sentinel finish oplog entry, or
     *  - hitting the end of the donor's oplog.
     *
     * Returns true if there are more oplog entries to be copied, and returns false if the sentinel
     * finish oplog entry has been copied.
     */
    bool consume(Client* client, CancelableOperationContextFactory factory, Shard* shard);

    bool iterate(Client* client, CancelableOperationContextFactory factory);

    int getNumOplogEntriesCopied() const {
        return _numOplogEntriesCopied;
    }

    ReshardingDonorOplogId getLastSeenTimestamp() const {
        return _startAt;
    }

    void setInitialBatchSizeForTest(int size) {
        _initialBatchSize = size;
    }

    void useReadConcernForTest(bool use) {
        _useReadConcern = use;
    }

    void setMaxBatchesForTest(int maxBatches) {
        _maxBatches = maxBatches;
    }

private:
    /**
     * Returns true if there's more work to do and the task should be rescheduled.
     */
    void _ensureCollection(Client* client,
                           CancelableOperationContextFactory factory,
                           NamespaceString nss);
    AggregateCommandRequest _makeAggregateCommandRequest(Client* client,
                                                         CancelableOperationContextFactory factory);
    ExecutorFuture<void> _reschedule(std::shared_ptr<executor::TaskExecutor> executor,
                                     const CancellationToken& cancelToken,
                                     CancelableOperationContextFactory factory);

    ServiceContext* _service() const {
        return _env->service();
    }

    std::unique_ptr<Env> _env;

    const UUID _reshardingUUID;
    const UUID _collUUID;
    ReshardingDonorOplogId _startAt;
    const ShardId _donorShard;
    const ShardId _recipientShard;
    const NamespaceString _toWriteInto;

    int _numOplogEntriesCopied = 0;

    Mutex _mutex = MONGO_MAKE_LATCH("ReshardingOplogFetcher::_mutex");
    Promise<void> _onInsertPromise;
    Future<void> _onInsertFuture;

    // For testing to control behavior.

    // The aggregation batch size. This only affects the original call and not `getmore`s.
    int _initialBatchSize = 0;
    // Setting to false will omit the `afterClusterTime` and `majority` read concern.
    bool _useReadConcern = true;
    // Dictates how many batches get processed before returning control from a call to `consume`.
    int _maxBatches = -1;
};
}  // namespace mongo
