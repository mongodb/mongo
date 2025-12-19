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

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Maintains an association between shard IDs and a state that is carried across shard instances.
 *
 * All state objects are created on demand and retained until explicitly deleted.
 */
class MONGO_MOD_NEEDS_REPLACEMENT ShardSharedStateCache {
public:
    struct Stats {
        /**
         * Number of operations attempted. Increments only once per operation, regardless of the
         * amount of retries performed to complete the operation.
         */
        Atomic<std::int64_t> numOperationsAttempted;

        /**
         * Number of operations that had to be retried because of an error that had the label
         * 'SystemOverloaded'. A high amount of operations failing against the total amount of
         * retries would indicate insufficient backoff.
         */
        Atomic<std::int64_t> numOperationsRetriedAtLeastOnceDueToOverload;

        /**
         * Number of operations that eventually yielded a response without the 'SystemOverloaded'
         * label. A low amount of operations that end up succeeding after backing off could mean
         * that the backoff is not aggresive enough or too many retries are allowed.
         */
        Atomic<std::int64_t> numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded;

        /**
         * The total amount of retries performed that were in response to an error that had the
         * 'SystemOverloaded' label.
         */
        Atomic<std::int64_t> numRetriesDueToOverloadAttempted;

        /**
         * The total amount of requests that had the 'SystemOverloaded' error label in the error
         * response.
         */
        Atomic<std::int64_t> numOverloadErrorsReceived;

        /**
         * The total number of retries directed to this shard that did not select a server that had
         * previously returned an error with the `SystemOverloaded` error label.
         */
        Atomic<std::int64_t> numRetriesRetargetedDueToOverload;

        /**
         * The total amount of milliseconds waited due to backing off.
         */
        Atomic<std::int64_t> totalBackoffTimeMillis;

        /**
         * Appends the stats for the shard metrics.
         */
        void appendStats(BSONObjBuilder* bob) const;
    };

    /**
     * Represents the shared state for all instances of a Shard with the same ShardId.
     *
     * Shard instances are recreated during shard registry refreshes, but certain states
     * must persist across these recreations to ensure consistent behavior. This structure
     * encapsulates the state that is shared for all instances of a Shard with a specific
     * ShardId.
     *
     * The lifetime of this structure will match the addition and removal of a shard in the config.
     */
    struct State {
        State(double returnRate, double capacity) : retryBudget{returnRate, capacity} {}
        AdaptiveRetryStrategy::RetryBudget retryBudget;
        Stats stats = {};
    };

    void forgetShardState(const ShardId& shardId);
    std::shared_ptr<State> getShardState(const ShardId& shardId);

    static ShardSharedStateCache& get(ServiceContext* serviceContext);
    static ShardSharedStateCache& get(OperationContext* opCtx);

    /**
     * Invoked when the value of the server parameters 'ShardRetryTokenReturnRate' is updated.
     */
    static Status updateRetryBudgetReturnRate(double returnRate);

    /**
     * Invoked when the value of the server parameters 'ShardRetryTokenBucketCapacity' is updated.
     */
    static Status updateRetryBudgetCapacity(std::int32_t capacity);

    /**
     * Report the metrics for all shards.
     */
    void report(BSONObjBuilder* bob) const;

private:
    void _updateRetryBudgetRateParameters(double returnRate, double capacity);

    mutable RWMutex _mutex;
    stdx::unordered_map<ShardId, std::shared_ptr<State>> _shardStateById;
};

}  // namespace mongo
