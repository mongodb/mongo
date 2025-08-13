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

#include "mongo/base/status_with.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/background.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>

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
                           NamespaceString oplogBufferNss,
                           bool storeProgress);

    ~ReshardingOplogFetcher() override;

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
                                  const CancellationToken& cancelToken);

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

    /**
     * Notifies the fetcher that oplog application has started.
     */
    void onStartingOplogApplication();

    /**
     * Makes the oplog fetcher prepare for the critical section. Currently, this makes the fetcher
     * start doing the following to reduce the likelihood of not finishing oplog fetching within the
     * critical section timeout:
     * - Start fetching oplog entries from the primary node instead of the "nearest" node which
     *   could be a lagged secondary.
     * - Sleep for reshardingOplogFetcherSleepMillisDuringCriticalSection instead of
     *   reshardingOplogFetcherSleepMillisBeforeCriticalSection after exhausting the oplog entries
     *   returned by the previous cursor.
     */
    void prepareForCriticalSection();

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
                                     const CancellationToken& cancelToken);

    /**
     * Returns true if the recipient has been configured to estimate the remaining time based on
     * the exponential moving average of the time it takes to fetch and apply oplog entries.
     */
    bool _needToEstimateRemainingTimeBasedOnMovingAverage(OperationContext* opCtx);

    /**
     * Returns true if the average for time to apply oplog entries needs to be updated, i.e. if the
     * recipient is in the 'apply' state and it has been more than
     * 'reshardingExponentialMovingAverageTimeToFetchAndApplyIntervalMillis' since the last update
     * which is when the last progress mark oplog entry was inserted.
     */
    bool _needToUpdateAverageTimeToApply(WithLock, OperationContext* opCtx) const;

    /**
     * If a progress mark oplog entry with the given timestamp needs to be inserted, returns an
     * oplog id for it. Otherwise, returns none.
     */
    boost::optional<ReshardingDonorOplogId> _makeProgressMarkOplogIdIfNeedToInsert(
        OperationContext* opCtx, const Timestamp& currBatchLastOplogTs);

    /**
     * Returns a progress mark noop oplog entry with the given oplog id.
     */
    repl::MutableOplogEntry _makeProgressMarkOplog(OperationContext* opCtx,
                                                   const ReshardingDonorOplogId& oplogId) const;

    ServiceContext* _service() const {
        return _env->service();
    }

    std::unique_ptr<Env> _env;

    const UUID _reshardingUUID;
    const UUID _collUUID;
    const ShardId _donorShard;
    const ShardId _recipientShard;
    const NamespaceString _oplogBufferNss;
    const bool _storeProgress;
    boost::optional<bool> _supportEstimatingRemainingTimeBasedOnMovingAverage;

    int _numOplogEntriesCopied = 0;
    AtomicWord<bool> _oplogApplicationStarted{false};

    // The mutex that protects all the members below.
    mutable stdx::mutex _mutex;
    ReshardingDonorOplogId _startAt;
    boost::optional<Date_t> _lastUpdatedProgressMarkAt;
    Promise<void> _onInsertPromise;
    Future<void> _onInsertFuture;
    AtomicWord<bool> _isPreparingForCriticalSection;
    // The cancellation source for the current aggregation.
    boost::optional<CancellationSource> _aggCancelSource;

    // For testing to control behavior.

    // The aggregation batch size. This only affects the original call and not `getmore`s.
    int _initialBatchSize = 0;
    // Setting to false will omit the `afterClusterTime` and `majority` read concern.
    bool _useReadConcern = true;
    // Dictates how many batches get processed before returning control from a call to `consume`.
    int _maxBatches = -1;
};
}  // namespace mongo
