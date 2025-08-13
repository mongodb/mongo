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

#include "mongo/db/namespace_string.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

#include <memory>
#include <vector>

namespace mongo {
namespace resharding {

/**
 * Used by the coordinator of a resharding operation to wait until the critical section can be
 * entered and exited within same predefined threshold. The monitoring is achieved by periodically
 * querying donor and recipient shards.
 *
 * The threshold is decided at the time of constructing a new `CoordinatorCommitMonitor`, and by
 * atomically fetching the value of `remainingReshardingOperationTimeThresholdMillis`.
 *
 * A cancellation token (i.e., `cancelToken`) is provided to each instance of
 * `CoordinatorCommitMonitor` and may be used for interruption. Once the monitor is interrupted, it
 * will make the returned `SemiFuture` ready.
 *
 * The monitor absorbs any internal error (e.g., cancellation errors) and always returns a ready
 * future to guarantee execution of any future chained to the returned `SemiFuture`.
 *
 * Internally, the monitor relies on the provided task executor to fulfill all its duties.
 */
class CoordinatorCommitMonitor : public std::enable_shared_from_this<CoordinatorCommitMonitor> {
public:
    using TaskExecutorPtr = std::shared_ptr<executor::TaskExecutor>;

    struct RemainingOperationTimes {
        Milliseconds min;
        Milliseconds max;
    };

    CoordinatorCommitMonitor(std::shared_ptr<ReshardingMetrics> metrics,
                             NamespaceString ns,
                             std::vector<ShardId> donorShards,
                             std::vector<ShardId> recipientShards,
                             TaskExecutorPtr executor,
                             CancellationToken cancelToken,
                             int delayBeforeInitialQueryMillis);

    SemiFuture<void> waitUntilRecipientsAreWithinCommitThreshold() const;

    /*
     * Allows passing a separate executor to `AsyncRequestsSender` to schedule remote requests. This
     * is necessary as fetching responses from `AsyncRequestsSender` is blocking (i.e., blocks the
     * executor thread), which does not confirm with our mocked networking infrastructure. Making
     * the fetch interface (i.e., `AsyncRequestsSender::next()`) asynchronous obviates the need for
     * this interface. Note that this is a test-only interface and may only be used in unit-tests.
     */
    void setNetworkExecutorForTest(TaskExecutorPtr networkExecutor);

    RemainingOperationTimes queryRemainingOperationTime() const;

private:
    ExecutorFuture<void> _makeFuture(Milliseconds delayBetweenQueries) const;

    static constexpr auto kDiagnosticLogLevel = 0;

    std::shared_ptr<ReshardingMetrics> _metrics;
    const NamespaceString _ns;
    const std::set<ShardId> _donorShards;
    const std::set<ShardId> _recipientShards;
    const std::set<ShardId> _participantShards;
    const TaskExecutorPtr _executor;
    const CancellationToken _cancelToken;

    const Milliseconds _delayBeforeInitialQueryMillis;

    TaskExecutorPtr _networkExecutor;
};

}  // namespace resharding
}  // namespace mongo
