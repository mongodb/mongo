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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

#include "mongo/db/query/query_stats/query_stats.h"

#include "mongo/crypto/hash_block.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/query_stats_on_parameter_change.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_clock_source.h"
#include <optional>

namespace mongo::query_stats {

Counter64 queryStatsStoreSizeEstimateBytesMetric;
ServerStatusMetricField<Counter64> displaySizeEstimateMetric(
    "queryStats.queryStatsStoreSizeEstimateBytes", &queryStatsStoreSizeEstimateBytesMetric);


const Decorable<ServiceContext>::Decoration<std::unique_ptr<QueryStatsStoreManager>>
    QueryStatsStoreManager::get =
        ServiceContext::declareDecoration<std::unique_ptr<QueryStatsStoreManager>>();

const Decorable<ServiceContext>::Decoration<std::unique_ptr<RateLimiting>>
    QueryStatsStoreManager::getRateLimiter =
        ServiceContext::declareDecoration<std::unique_ptr<RateLimiting>>();


namespace {

Counter64 queryStatsEvictedMetric;
ServerStatusMetricField<Counter64> displayEvictedMetric("queryStats.numEvicted",
                                                        &queryStatsEvictedMetric);
Counter64 queryStatsRateLimitedRequestsMetric;
ServerStatusMetricField<Counter64> displayRateLimitMetric("queryStats.numRateLimitedRequests",
                                                          &queryStatsRateLimitedRequestsMetric);
Counter64 queryStatsStoreWriteErrorsMetric;
ServerStatusMetricField<Counter64> displayWriteErrorsMetric(
    "queryStats.numQueryStatsStoreWriteErrors", &queryStatsStoreWriteErrorsMetric);

/**
 * Indicates whether or not query stats is enabled via the feature flag.
 */
bool isQueryStatsFeatureEnabled() {
    // We need to call isVersionInitialized() first because this could run during startup while the
    // FCV is still uninitialized.
    if (serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        return feature_flags::gFeatureFlagQueryStats.isEnabled(
            serverGlobalParams.featureCompatibility);
    }
    // (Generic FCV reference): This reference is needed to ensure we correctly initialize query
    // stats during startup.
    return feature_flags::gFeatureFlagQueryStats.isEnabledOnVersion(
        multiversion::GenericFCV::kLatest);
}

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

void assertConfigurationAllowed() {
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "Cannot configure queryStats store. The feature flag is not enabled. Please restart "
            "and specify the feature flag, or upgrade the feature compatibility version to one "
            "where it is enabled by default.",
            isQueryStatsFeatureEnabled());
}

class QueryStatsOnParamChangeUpdaterImpl final : public query_stats_util::OnParamChangeUpdater {
public:
    void updateCacheSize(ServiceContext* serviceCtx, memory_util::MemorySize memSize) final {
        assertConfigurationAllowed();
        auto requestedSize = memory_util::convertToSizeInBytes(memSize);
        auto cappedSize = capQueryStatsStoreSize(requestedSize);
        auto& queryStatsStoreManager = QueryStatsStoreManager::get(serviceCtx);
        size_t numEvicted = queryStatsStoreManager->resetSize(cappedSize);
        queryStatsEvictedMetric.increment(numEvicted);
    }

    void updateSamplingRate(ServiceContext* serviceCtx, int samplingRate) {
        assertConfigurationAllowed();
        QueryStatsStoreManager::getRateLimiter(serviceCtx).get()->setSamplingRate(samplingRate);
    }
};

ServiceContext::ConstructorActionRegisterer queryStatsStoreManagerRegisterer{
    "QueryStatsStoreManagerRegisterer", [](ServiceContext* serviceCtx) {
        // Note: it is possible that this is called before FCV is properly set up. The feature flags
        // can only be specified at startup, but the feature compatibility version may change at
        // runtime. If the feature compatibility version upgrades at runtime, the feature may now be
        // enabled by default, even if the flag was not specified. To allow for this possibility, we
        // will always configure a query stats store of the size currently specified by
        // 'internalQueryStatsCacheSize', but we will prevent changing its shape or rate limit at
        // runtime unless the feature flag is enabled (at whatever current FCV when the
        // configuration setParameter command is run).

        query_stats_util::queryStatsStoreOnParamChangeUpdater(serviceCtx) =
            std::make_unique<QueryStatsOnParamChangeUpdaterImpl>();
        size_t size = getQueryStatsStoreSize();
        auto&& globalQueryStatsStoreManager = QueryStatsStoreManager::get(serviceCtx);
        // Initially the queryStats store used the same number of partitions as the plan cache, that
        // is the number of cpu cores. However, with performance investigation we found that when
        // the size of the partitions was too large, it took too long to copy out and read one
        // partition. We are now capping each partition at 16MB (the largest size a query shape can
        // be. If that gives us fewer partitions than we have cores, we set it to match the
        // number of cores. The size needs to be cast to a double since we want to round up the
        // number of partitions, and therefore need to avoid int division.
        size_t numPartitions = std::ceil(double(size) / (16 * 1024 * 1024));
        auto numLogicalCores = ProcessInfo::getNumCores();
        if (numPartitions < numLogicalCores) {
            numPartitions = numLogicalCores;
        }

        globalQueryStatsStoreManager =
            std::make_unique<QueryStatsStoreManager>(size, numPartitions);
        auto configuredSamplingRate = internalQueryStatsRateLimit.load();
        QueryStatsStoreManager::getRateLimiter(serviceCtx) = std::make_unique<RateLimiting>(
            configuredSamplingRate < 0 ? INT_MAX : configuredSamplingRate, Seconds{1});
    }};

/**
 * Top-level checks for whether queryStats collection is enabled. If this returns false, we must
 * go no further.
 */
bool isQueryStatsEnabled(const ServiceContext* serviceCtx) {
    // During initialization, FCV may not yet be setup but queries could be run. We can't
    // check whether queryStats should be enabled without FCV, so default to not recording
    // those queries.
    return isQueryStatsFeatureEnabled() &&
        QueryStatsStoreManager::get(serviceCtx)->getMaxSize() > 0;
}

/**
 * Internal check for whether we should collect metrics. This checks the rate limiting
 * configuration for a global on/off decision and, if enabled, delegates to the rate limiter.
 */
bool shouldCollect(const ServiceContext* serviceCtx) {
    // Cannot collect queryStats if sampling rate is not greater than 0. Note that we do not
    // increment queryStatsRateLimitedRequestsMetric here since queryStats is entirely disabled.
    auto samplingRate = QueryStatsStoreManager::getRateLimiter(serviceCtx)->getSamplingRate();
    if (samplingRate <= 0) {
        LOGV2_DEBUG(8473001,
                    5,
                    "sampling rate is <= 0, skipping this request",
                    "samplingRate"_attr = samplingRate);
        return false;
    }
    // Check if rate limiting allows us to collect queryStats for this request.
    if (samplingRate < INT_MAX &&
        !QueryStatsStoreManager::getRateLimiter(serviceCtx)->handleRequestSlidingWindow()) {
        queryStatsRateLimitedRequestsMetric.increment();
        LOGV2_DEBUG(8473002,
                    5,
                    "rate limited this request",
                    "samplingRate"_attr = samplingRate,
                    "totalLimited"_attr = queryStatsRateLimitedRequestsMetric.get());
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

void registerRequest(OperationContext* opCtx,
                     const NamespaceString& collection,
                     std::function<std::unique_ptr<Key>(void)> makeKey,
                     bool willNeverExhaust) {
    if (!isQueryStatsEnabled(opCtx->getServiceContext())) {
        LOGV2_DEBUG(8473000,
                    5,
                    "not collecting query stats for this request since it is disabled",
                    "featureEnabled"_attr = isQueryStatsFeatureEnabled());
        return;
    }

    // Queries against metadata collections should never appear in queryStats data.
    if (collection.isFLE2StateCollection()) {
        return;
    }

    // Don't record queries from internal clients.
    if (opCtx->getClient()->session() &&
        (opCtx->getClient()->session()->getTags() & transport::Session::kInternalClient)) {
        return;
    }

    auto& opDebug = CurOp::get(opCtx)->debug();

    if (opDebug.queryStatsInfo.wasRateLimited) {
        LOGV2_DEBUG(
            8288900,
            4,
            "Query stats request was previously rate limited. We expect this is a query on a view");
        return;
    }

    if (!shouldCollect(opCtx->getServiceContext())) {
        opDebug.queryStatsInfo.wasRateLimited = true;
        return;
    }

    if (opDebug.queryStatsInfo.key) {
        // A find() request may have already registered the shapifier. Ie, it's a find command over
        // a non-physical collection, eg view, which is implemented by generating an agg pipeline.
        LOGV2_DEBUG(7198700,
                    2,
                    "Query stats request shapifier already registered",
                    "collection"_attr = collection);
        return;
    }

    opDebug.queryStatsInfo.willNeverExhaust = willNeverExhaust;
    // There are a few cases where a query shape can be larger than the original query. For example,
    // {$exists: false} in the input query serializes to {$not: {$exists: true}. In rare cases where
    // an input query has thousands of clauses, the cumulative bloat that shapification adds results
    // in a BSON object that exceeds the 16 MB memory limit. In these cases, we want to exclude the
    // original query from queryStats metrics collection and let it execute normally.
    try {
        opDebug.queryStatsInfo.key = makeKey();
    } catch (const DBException& ex) {
        queryStatsStoreWriteErrorsMetric.increment();

        const auto status = ex.toStatus();
        if (status.code() == ErrorCodes::BSONObjectTooLarge) {
            LOGV2_DEBUG(7979400,
                        2,
                        "Query Stats shapification has exceeded the 16 MB memory limit. Metrics "
                        "will not be collected");
            return;
        }

        const auto& cmdObj = CurOp::get(opCtx)->opDescription();
        LOGV2_DEBUG(9423100,
                    2,
                    "Error encountered when creating the Query Stats store key. Metrics will not "
                    "be collected for this command",
                    "status"_attr = status,
                    "command"_attr = cmdObj);
        if (kDebugBuild || internalQueryStatsErrorsAreCommandFatal.load()) {
            // uassert rather than tassert so that we avoid creating fatal failures on queries that
            // were going to fail anyway, but trigger the error here first. A query that ONLY fails
            // when query stats is enabled will still be surfaced by the uassert.
            // Note that in the former case, these queries will fail with a different error code
            // than they would have otherwise. Since this block is only applicable in test
            // environments, this is fine. We make this tradeoff because it is desirable to have
            // real bugs clearly surfaced as query stats issues.
            uasserted(9423101,
                      str::stream() << "Failed to create query stats store key. Status: " << status
                                    << " Command: " << cmdObj);
        }

        return;
    }
    opDebug.queryStatsInfo.keyHash = absl::Hash<query_stats::Key>{}(*opDebug.queryStatsInfo.key);
    // TODO look up this query shape (sub-component of query stats store key) in some new shared
    // data structure that the query settings component could share. See if the query SHAPE hash has
    // been computed before. If so, record the query shape hash on the opDebug. If not, compute the
    // hash and store it there so we can avoid re-doing this for each request.
}

QueryStatsStore& getQueryStatsStore(OperationContext* opCtx) {
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "Query stats is not enabled without the feature flag on and a cache size greater than "
            "0 bytes",
            isQueryStatsEnabled(opCtx->getServiceContext()));
    return QueryStatsStoreManager::get(opCtx->getServiceContext())->getQueryStatsStore();
}

void writeQueryStats(OperationContext* opCtx,
                     boost::optional<size_t> queryStatsKeyHash,
                     std::unique_ptr<Key> key,
                     const uint64_t queryExecMicros,
                     const uint64_t firstResponseExecMicros,
                     const uint64_t docsReturned,
                     bool willNeverExhaust) {
    // Generally we expect a 'key' to write query stats. However, for a change stream query, we
    // expect it has no 'key' after its first writeQueryStats(), but it must have a
    // 'queryStatsKeyHash' for its entry to be updated.
    // TODO SERVER-89058 Modify comment to include tailable cursors.
    if (!key && !(willNeverExhaust && queryStatsKeyHash)) {
        return;
    }

    // It's possible that query stats was enabled in registerRequest but has been disabled since
    // (e.g., by FCV downgrade or setting the store size to 0). Rather than calling
    // getQueryStatsStore (which would trigger a uassert if queryStats is disabled), we return and
    // log a message if query stats is disabled, and otherwise grab the query stats store directly.
    if (!isQueryStatsEnabled(opCtx->getServiceContext())) {
        LOGV2_DEBUG(8456700,
                    2,
                    "Query stats was enabled when the command started but is now disabled. "
                    "Metrics will not be collected.",
                    "queryStatsKeyHash"_attr = queryStatsKeyHash);
        return;
    }
    auto&& queryStatsStore =
        QueryStatsStoreManager::get(opCtx->getServiceContext())->getQueryStatsStore();
    if (key) {
        dassert(absl::Hash<query_stats::Key>{}(*key) == queryStatsKeyHash,
                "Expecting query stats key to hash to the given hash. Is the OpCtx state being "
                "incorrectly re-used?");
    }
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

    // It is possible a cursor that lives forever has no key associated with it and its entry may
    // have been evicted.
    if (willNeverExhaust && !key) {
        return;
    }

    // Otherwise we didn't find an existing entry. Try to create one.
    tassert(7315200,
            "key cannot be null when writing a new entry to the queryStats store",
            key != nullptr);
    size_t numEvicted =
        queryStatsStore.put(*queryStatsKeyHash, QueryStatsEntry(std::move(key)), partitionLock);
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

void writeQueryStatsOnCursorDisposeOrKill(OperationContext* opCtx,
                                          boost::optional<size_t> queryStatsKeyHash,
                                          std::unique_ptr<Key> key,
                                          bool willNeverExhaust,
                                          const uint64_t queryExecMicros,
                                          const uint64_t firstResponseExecMicros,
                                          const uint64_t docsReturned) {
    // It is discouraged but technically possible for a user to enable queryStats on the mongods of
    // a replica set. In this case, a cursor will be created for each mongod. However, the
    // queryStatsKey is behind a unique_ptr on CurOp. The ClientCursor constructor std::moves the
    // queryStatsKey so it uniquely owns it (and also makes the queryStatsKey on CurOp now a
    // nullptr) and copies over the queryStatsKeyHash as the latter is a cheap copy.
    // In the case of sharded $search, two cursors will be created per mongod. In this way,
    // two cursors are part of the same thread/operation, and therefore share a OpCtx/CurOp/OpDebug.
    // The first cursor that is created will own the queryStatsKey and have a copy of the
    // queryStatsKeyHash. On the other hand, the second one will only have a copy of the hash since
    // the queryStatsKey will be null on CurOp from being std::move'd in the first cursor
    // construction call. To not trip the tassert in writeQueryStats and because all cursors are
    // guaranteed to have a copy of the hash, we check that the cursor has a key
    if (key && opCtx) {
        query_stats::writeQueryStats(opCtx,
                                     queryStatsKeyHash,
                                     std::move(key),
                                     queryExecMicros,
                                     firstResponseExecMicros,
                                     docsReturned,
                                     willNeverExhaust);
    } else if (willNeverExhaust && opCtx) {
        // Since we already recorded information about the possible getMores associated with a
        // cursor that never ends, the only information left to record is about the kill/dispose
        // cursor operation. This operation is not timed and does not have any metrics associated
        // with it.
        query_stats::writeQueryStats(opCtx, queryStatsKeyHash, nullptr, 0, 0, 0, willNeverExhaust);
    }
}

}  // namespace mongo::query_stats
