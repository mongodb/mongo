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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {
namespace analyze_shard_key {

/**
 * Owns the machinery for sampling queries on a sampler. That consists of the following:
 * - The periodic background job that refreshes the last exponential moving average of the number of
 *   queries that this sampler executes per second.
 * - The periodic background job that sends the calculated average to the coordinator to refresh the
 *   latest configurations. The average determines the share of the cluster-wide sample rate that
 *   will be assigned to this sampler.
 * - The rate limiters that each controls the rate at which queries against a collection are sampled
 *   on this sampler.
 *
 * Currently, query sampling is only supported on a sharded cluster. So a sampler must be a mongos
 * and the coordinator must be the config server's primary mongod.
 */
class QueryAnalysisSampler final {
    QueryAnalysisSampler(const QueryAnalysisSampler&) = delete;
    QueryAnalysisSampler& operator=(const QueryAnalysisSampler&) = delete;

public:
    /**
     * Stores the last total number of queries that this sampler has executed and the last
     * exponential moving average number of queries that this sampler executes per second. The
     * average is recalculated every second when the total number of queries is refreshed.
     */
    struct QueryStats {
        QueryStats() = default;

        long long getLastTotalCount() const {
            return _lastTotalCount;
        }

        boost::optional<double> getLastAvgCount() const {
            return _lastAvgCount;
        }

        /**
         * Refreshes the last total count and the last exponential moving average count. To be
         * invoked every second.
         */
        void refreshTotalCount(long long newTotalCount);

    private:
        double _calculateExponentialMovingAverage(double prevAvg, long long newVal) const;

        const double _smoothingFactor = gQueryAnalysisQueryStatsSmoothingFactor;
        long long _lastTotalCount = 0;
        boost::optional<double> _lastAvgCount;
    };

    /**
     * Controls the per-second rate at which queries against a collection are sampled on this
     * sampler. Uses token bucket.
     */
    class SampleRateLimiter {
    public:
        static constexpr double kEpsilon = 0.001;

        SampleRateLimiter(ServiceContext* serviceContext,
                          const NamespaceString& nss,
                          const UUID& collUuid,
                          double numTokensPerSecond)
            : _serviceContext(serviceContext),
              _nss(nss),
              _collUuid(collUuid),
              _numTokensPerSecond(numTokensPerSecond) {
            invariant(_numTokensPerSecond > 0);
            _lastRefillTime = _serviceContext->getFastClockSource()->now();
        };

        const NamespaceString& getNss() const {
            return _nss;
        }

        const UUID& getCollectionUuid() const {
            return _collUuid;
        }

        double getRate() const {
            return _numTokensPerSecond;
        }

        double getBurstCapacity() const {
            return _getBurstCapacity(_numTokensPerSecond);
        }

        /**
         * Requests to consume one token from the bucket. Causes the bucket to be refilled with
         * tokens created since last refill time. Does not block if the bucket is empty or there is
         * fewer than one token in the bucket. Returns true if a token has been consumed
         * successfully, and false otherwise.
         */
        bool tryConsume();

        /**
         * Sets a new rate. Causes the bucket to be refilled with tokens created since last refill
         * time according to the previous rate.
         */
        void refreshRate(double numTokensPerSecond);

    private:
        /**
         * Returns the maximum of number of tokens that a bucket with given rate can store at any
         * given time.
         */
        static double _getBurstCapacity(double numTokensPerSecond);

        /**
         * Fills the bucket with tokens created since last refill time according to the given rate
         * and burst capacity.
         */
        void _refill(double numTokensPerSecond, double burstCapacity);

        const ServiceContext* _serviceContext;
        const NamespaceString _nss;
        const UUID _collUuid;
        double _numTokensPerSecond;

        // The bucket is only refilled when there is a consume request or a rate refresh.
        Date_t _lastRefillTime;
        double _lastNumTokens = 0;
    };

    QueryAnalysisSampler() = default;
    ~QueryAnalysisSampler() = default;

    QueryAnalysisSampler(QueryAnalysisSampler&& source) = delete;
    QueryAnalysisSampler& operator=(QueryAnalysisSampler&& other) = delete;

    /**
     * Obtains the service-wide QueryAnalysisSampler instance.
     */
    static QueryAnalysisSampler& get(OperationContext* opCtx);
    static QueryAnalysisSampler& get(ServiceContext* serviceContext);

    void onStartup();

    void onShutdown();

    /**
     * Returns true if a query should be sampled, and false otherwise. Can only be invoked once on
     * for each query since it decrements the number remaining queries to sample if this query
     * should be sampled.
     */
    bool shouldSample(const NamespaceString& nss);

    void refreshQueryStatsForTest() {
        _refreshQueryStats();
    }

    QueryStats getQueryStatsForTest() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _queryStats;
    }

    void refreshConfigurationsForTest(OperationContext* opCtx) {
        _refreshConfigurations(opCtx);
    }

    std::map<NamespaceString, SampleRateLimiter> getRateLimitersForTest() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _sampleRateLimiters;
    }

private:
    void _refreshQueryStats();

    void _refreshConfigurations(OperationContext* opCtx);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("QueryAnalysisSampler::_mutex");

    PeriodicJobAnchor _periodicQueryStatsRefresher;
    QueryStats _queryStats;

    PeriodicJobAnchor _periodicConfigurationsRefresher;
    std::map<NamespaceString, SampleRateLimiter> _sampleRateLimiters;
};

}  // namespace analyze_shard_key
}  // namespace mongo
