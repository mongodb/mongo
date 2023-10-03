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

#include "mongo/db/query/query_stats/query_stats.h"

#include <absl/container/node_hash_map.h>
#include <absl/hash/hash.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <climits>
#include <list>

#include "mongo/base/status_with.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/db/catalog/util/partitioned.h"
#include "mongo/db/curop.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/util.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::query_stats {

CounterMetric queryStatsStoreSizeEstimateBytesMetric("queryStats.queryStatsStoreSizeEstimateBytes");

namespace {

CounterMetric queryStatsEvictedMetric("queryStats.numEvicted");
CounterMetric queryStatsRateLimitedRequestsMetric("queryStats.numRateLimitedRequests");
CounterMetric queryStatsStoreWriteErrorsMetric("queryStats.numQueryStatsStoreWriteErrors");


/**
 * Cap the queryStats store size.
 */
size_t capQueryStatsStoreSize(size_t requestedSize) {
    size_t cappedStoreSize = memory_util::capMemorySize(
        requestedSize /*requestedSizeBytes*/, 1 /*maximumSizeGB*/, 25 /*percentTotalSystemMemory*/);
    // If capped size is less than requested size, the queryStats store has been capped at its
    // upper limit.
    if (cappedStoreSize < requestedSize) {
        LOGV2_DEBUG(7106502,
                    1,
                    "The queryStats store size has been capped",
                    "cappedSize"_attr = cappedStoreSize);
    }
    return cappedStoreSize;
}

/**
 * Get the queryStats store size based on the query job's value.
 */
size_t getQueryStatsStoreSize() {
    auto status = memory_util::MemorySize::parse(internalQueryStatsCacheSize.get());
    uassertStatusOK(status);
    size_t requestedSize = memory_util::convertToSizeInBytes(status.getValue());
    return capQueryStatsStoreSize(requestedSize);
}

class TelemetryOnParamChangeUpdaterImpl final : public query_stats_util::OnParamChangeUpdater {
public:
    void updateCacheSize(ServiceContext* serviceCtx, memory_util::MemorySize memSize) final {
        auto requestedSize = memory_util::convertToSizeInBytes(memSize);
        auto cappedSize = capQueryStatsStoreSize(requestedSize);
        auto& queryStatsStoreManager = queryStatsStoreDecoration(serviceCtx);
        size_t numEvicted = queryStatsStoreManager->resetSize(cappedSize);
        queryStatsEvictedMetric.increment(numEvicted);
    }

    void updateSamplingRate(ServiceContext* serviceCtx, int samplingRate) {
        queryStatsRateLimiter(serviceCtx).get()->setSamplingRate(samplingRate);
    }
};

ServiceContext::ConstructorActionRegisterer queryStatsStoreManagerRegisterer{
    "QueryStatsStoreManagerRegisterer", [](ServiceContext* serviceCtx) {
        // It is possible that this is called before FCV is properly set up. Setting up the store if
        // the flag is enabled but FCV is incorrect is safe, and guards against the FCV being
        // changed to a supported version later.
        if (!feature_flags::gFeatureFlagQueryStats.isEnabledAndIgnoreFCVUnsafeAtStartup() &&
            !feature_flags::gFeatureFlagQueryStatsFindCommand
                 .isEnabledAndIgnoreFCVUnsafeAtStartup()) {
            // featureFlags are not allowed to be changed at runtime. Therefore it's not an issue
            // to not create a queryStats store in ConstructorActionRegisterer at start up with the
            // flag off - because the flag can not be turned on at any point afterwards.
            query_stats_util::queryStatsStoreOnParamChangeUpdater(serviceCtx) =
                std::make_unique<query_stats_util::NoChangesAllowedTelemetryParamUpdater>();
            return;
        }

        query_stats_util::queryStatsStoreOnParamChangeUpdater(serviceCtx) =
            std::make_unique<TelemetryOnParamChangeUpdaterImpl>();
        size_t size = getQueryStatsStoreSize();
        auto&& globalQueryStatsStoreManager = queryStatsStoreDecoration(serviceCtx);
        // Initially the queryStats store used the same number of partitions as the plan cache, that
        // is the number of cpu cores. However, with performance investigation we found that when
        // the size of the partitions was too large, it took too long to copy out and read one
        // partition. We are now capping each partition at 16MB (the largest size a query shape can
        // be), or smaller if that gives us fewer partitions than we have cores. The size needs to
        // be cast to a double since we want to round up the number of partitions, and therefore
        // need to avoid int division.
        size_t numPartitions = std::ceil(double(size) / (16 * 1024 * 1024));
        // This is our guess at how big a small-ish query shape (+ metrics) would be, but
        // intentionally not the smallest possible one. The purpose of this constant is to keep us
        // from making each partition so small that it does not record anything, while still being
        // small enough to allow us to shrink the overall memory footprint of the data structure if
        // the user requested that we do so.
        constexpr double approxEntrySize = 0.004 * 1024 * 1024;  // 4KB
        if (numPartitions < ProcessInfo::getNumCores()) {
            numPartitions = std::ceil(double(size) / (approxEntrySize * 10));
        }

        globalQueryStatsStoreManager =
            std::make_unique<QueryStatsStoreManager>(size, numPartitions);
        auto configuredSamplingRate = internalQueryStatsRateLimit.load();
        queryStatsRateLimiter(serviceCtx) = std::make_unique<RateLimiting>(
            configuredSamplingRate < 0 ? INT_MAX : configuredSamplingRate, Seconds{1});
    }};

/**
 * Top-level checks for whether queryStats collection is enabled. If this returns false, we must go
 * no further.
 * TODO SERVER-79494 Remove requiresFullQueryStatsFeatureFlag parameter.
 */
bool isQueryStatsEnabled(const ServiceContext* serviceCtx, bool requiresFullQueryStatsFeatureFlag) {
    // During initialization, FCV may not yet be setup but queries could be run. We can't
    // check whether queryStats should be enabled without FCV, so default to not recording
    // those queries.
    // TODO SERVER-75935 Remove FCV Check.
    return isQueryStatsFeatureEnabled(requiresFullQueryStatsFeatureFlag) &&
        queryStatsStoreDecoration(serviceCtx)->getMaxSize() > 0;
}

/**
 * Internal check for whether we should collect metrics. This checks the rate limiting
 * configuration for a global on/off decision and, if enabled, delegates to the rate limiter.
 */
bool shouldCollect(const ServiceContext* serviceCtx) {
    // Cannot collect queryStats if sampling rate is not greater than 0. Note that we do not
    // increment queryStatsRateLimitedRequestsMetric here since queryStats is entirely disabled.
    auto samplingRate = queryStatsRateLimiter(serviceCtx)->getSamplingRate();
    if (samplingRate <= 0) {
        return false;
    }
    // Check if rate limiting allows us to collect queryStats for this request.
    if (samplingRate < INT_MAX &&
        !queryStatsRateLimiter(serviceCtx)->handleRequestSlidingWindow()) {
        queryStatsRateLimitedRequestsMetric.increment();
        return false;
    }
    return true;
}

void updateStatistics(const QueryStatsStore::Partition& proofOfLock,
                      QueryStatsEntry& toUpdate,
                      const uint64_t queryExecMicros,
                      const uint64_t firstResponseExecMicros,
                      const uint64_t docsReturned) {
    toUpdate.latestSeenTimestamp = Date_t::now();
    toUpdate.lastExecutionMicros = queryExecMicros;
    toUpdate.execCount++;
    toUpdate.totalExecMicros.aggregate(queryExecMicros);
    toUpdate.firstResponseExecMicros.aggregate(firstResponseExecMicros);
    toUpdate.docsReturned.aggregate(docsReturned);
}

}  // namespace

/**
 * Indicates whether or not query stats is enabled via the feature flags. If
 * requiresFullQueryStatsFeatureFlag is true, it will only return true if featureFlagQueryStats is
 * enabled. Otherwise, it will return true if either featureFlagQueryStats or
 * featureFlagQueryStatsFindCommand is enabled.
 *
 * TODO SERVER-79494 Remove this function and collapse feature flag check into isQueryStatsEnabled.
 */
bool isQueryStatsFeatureEnabled(bool requiresFullQueryStatsFeatureFlag) {
    return feature_flags::gFeatureFlagQueryStats.isEnabled(
               serverGlobalParams.featureCompatibility) ||
        (!requiresFullQueryStatsFeatureFlag &&
         feature_flags::gFeatureFlagQueryStatsFindCommand.isEnabled(
             serverGlobalParams.featureCompatibility));
}

void registerRequest(OperationContext* opCtx,
                     const NamespaceString& collection,
                     std::function<std::unique_ptr<KeyGenerator>(void)> makeKeyGenerator,
                     bool requiresFullQueryStatsFeatureFlag) {
    if (!isQueryStatsEnabled(opCtx->getServiceContext(), requiresFullQueryStatsFeatureFlag)) {
        return;
    }

    // Queries against metadata collections should never appear in queryStats data.
    if (collection.isFLE2StateCollection()) {
        return;
    }

    if (!shouldCollect(opCtx->getServiceContext())) {
        return;
    }
    auto& opDebug = CurOp::get(opCtx)->debug();

    if (opDebug.queryStatsKeyGenerator) {
        // A find() request may have already registered the shapifier. Ie, it's a find command over
        // a non-physical collection, eg view, which is implemented by generating an agg pipeline.
        LOGV2_DEBUG(7198700,
                    2,
                    "Query stats request shapifier already registered",
                    "collection"_attr = collection);
        return;
    }
    // There are a few cases where a query shape can be larger than the original query. For example,
    // {$exists: false} in the input query serializes to {$not: {$exists: true}. In rare cases where
    // an input query has thousands of clauses, the cumulative bloat that shapification adds results
    // in a BSON object that exceeds the 16 MB memory limit. In these cases, we want to exclude the
    // original query from queryStats metrics collection and let it execute normally.
    try {
        opDebug.queryStatsKeyGenerator = makeKeyGenerator();
    } catch (ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
        LOGV2_DEBUG(7979400,
                    1,
                    "Query Stats shapification has exceeded the 16 MB memory limit. Metrics will "
                    "not be collected ");
        queryStatsStoreWriteErrorsMetric.increment();
        return;
    }
    opDebug.queryStatsStoreKeyHash = absl::HashOf(*opDebug.queryStatsKeyGenerator);
    // TODO look up this query shape (sub-component of query stats store key) in some new shared
    // data structure that the query settings component could share. See if the query SHAPE hash has
    // been computed before. If so, record the query shape hash on the opDebug. If not, compute the
    // hash and store it there so we can avoid re-doing this for each request.
}

QueryStatsStore& getQueryStatsStore(OperationContext* opCtx) {
    uassert(6579000,
            "Query stats is not enabled without the feature flag on and a cache size greater than "
            "0 bytes",
            isQueryStatsEnabled(opCtx->getServiceContext(),
                                /*requiresFullQueryStatsFeatureFlag*/ false));
    return queryStatsStoreDecoration(opCtx->getServiceContext())->getQueryStatsStore();
}

void writeQueryStats(OperationContext* opCtx,
                     boost::optional<size_t> queryStatsKeyHash,
                     std::unique_ptr<KeyGenerator> keyGenerator,
                     const uint64_t queryExecMicros,
                     const uint64_t firstResponseExecMicros,
                     const uint64_t docsReturned) {
    if (!queryStatsKeyHash) {
        return;
    }
    auto&& queryStatsStore = getQueryStatsStore(opCtx);
    auto&& [statusWithMetrics, partitionLock] =
        queryStatsStore.getWithPartitionLock(*queryStatsKeyHash);
    if (statusWithMetrics.isOK()) {
        // Found an existing entry! Just update the metrics and we're done.
        return updateStatistics(partitionLock,
                                *statusWithMetrics.getValue(),
                                queryExecMicros,
                                firstResponseExecMicros,
                                docsReturned);
    }

    // Otherwise we didn't find an existing entry. Try to create one.
    tassert(7315200,
            "keyGenerator cannot be null when writing a new entry to the telemetry store",
            keyGenerator != nullptr);
    size_t numEvicted = queryStatsStore.put(
        *queryStatsKeyHash, QueryStatsEntry(std::move(keyGenerator)), partitionLock);
    queryStatsEvictedMetric.increment(numEvicted);
    auto newMetrics = partitionLock->get(*queryStatsKeyHash);
    if (!newMetrics.isOK()) {
        // This can happen if the budget is immediately exceeded. Specifically if the there is
        // not enough room for a single new entry if the number of partitions is too high
        // relative to the size.
        queryStatsStoreWriteErrorsMetric.increment();
        LOGV2_DEBUG(7560900,
                    0,
                    "Failed to store queryStats entry.",
                    "status"_attr = newMetrics.getStatus(),
                    "queryStatsKeyHash"_attr = queryStatsKeyHash);
        return;
    }

    return updateStatistics(partitionLock,
                            newMetrics.getValue()->second,
                            queryExecMicros,
                            firstResponseExecMicros,
                            docsReturned);
}
}  // namespace mongo::query_stats
