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

#include "mongo/db/query/query_stats.h"

#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_stats_util.h"
#include "mongo/db/query/rate_limiting.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_clock_source.h"
#include <optional>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace query_stats {

/**
 * Redacts all BSONObj field names as if they were paths, unless the field name is a special hint
 * operator.
 */
namespace {

boost::optional<std::string> getApplicationName(const OperationContext* opCtx) {
    if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
        return metadata->getApplicationName().toString();
    }
    return boost::none;
}
}  // namespace

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

/**
 * A manager for the queryStats store allows a "pointer swap" on the queryStats store itself. The
 * usage patterns are as follows:
 *
 * - Updating the queryStats store uses the `getQueryStatsStore()` method. The queryStats store
 *   instance is obtained, entries are looked up and mutated, or created anew.
 * - The queryStats store is "reset". This involves atomically allocating a new instance, once
 * there are no more updaters (readers of the store "pointer"), and returning the existing
 * instance.
 */
class QueryStatsStoreManager {
public:
    template <typename... QueryStatsStoreArgs>
    QueryStatsStoreManager(size_t cacheSize, size_t numPartitions)
        : _queryStatsStore(std::make_unique<QueryStatsStore>(cacheSize, numPartitions)),
          _maxSize(cacheSize) {}

    /**
     * Acquire the instance of the queryStats store.
     */
    QueryStatsStore& getQueryStatsStore() {
        return *_queryStatsStore;
    }

    size_t getMaxSize() {
        return _maxSize;
    }

    /**
     * Resize the queryStats store and return the number of evicted
     * entries.
     */
    size_t resetSize(size_t cacheSize) {
        _maxSize = cacheSize;
        return _queryStatsStore->reset(cacheSize);
    }

private:
    std::unique_ptr<QueryStatsStore> _queryStatsStore;

    /**
     * Max size of the queryStats store. Tracked here to avoid having to recompute after it's
     * divided up into partitions.
     */
    size_t _maxSize;
};

const auto queryStatsStoreDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<QueryStatsStoreManager>>();

const auto queryStatsRateLimiter =
    ServiceContext::declareDecoration<std::unique_ptr<RateLimiting>>();

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
        if (!feature_flags::gFeatureFlagQueryStats.isEnabledAndIgnoreFCVUnsafeAtStartup()) {
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
        // The plan cache and queryStats store should use the same number of partitions.
        // That is, the number of cpu cores.
        size_t numPartitions = ProcessInfo::getNumCores();
        size_t partitionBytes = size / numPartitions;
        size_t metricsSize = sizeof(QueryStatsEntry);
        if (partitionBytes < metricsSize * 10) {
            numPartitions = size / metricsSize;
            if (numPartitions < 1) {
                numPartitions = 1;
            }
        }
        globalQueryStatsStoreManager =
            std::make_unique<QueryStatsStoreManager>(size, numPartitions);
        auto configuredSamplingRate = internalQueryStatsRateLimit.load();
        queryStatsRateLimiter(serviceCtx) = std::make_unique<RateLimiting>(
            configuredSamplingRate < 0 ? INT_MAX : configuredSamplingRate);
    }};

/**
 * Top-level checks for whether queryStats collection is enabled. If this returns false, we must go
 * no further.
 */
bool isQueryStatsEnabled(const ServiceContext* serviceCtx) {
    // During initialization FCV may not yet be setup but queries could be run. We can't
    // check whether queryStats should be enabled without FCV, so default to not recording
    // those queries.
    // TODO SERVER-75935 Remove FCV Check.
    return feature_flags::gFeatureFlagQueryStats.isEnabled(
               serverGlobalParams.featureCompatibility) &&
        queryStatsStoreDecoration(serviceCtx)->getMaxSize() > 0;
}

/**
 * Internal check for whether we should collect metrics. This checks the rate limiting
 * configuration for a global on/off decision and, if enabled, delegates to the rate limiter.
 */
bool shouldCollect(const ServiceContext* serviceCtx) {
    // Quick escape if queryStats is turned off.
    if (!isQueryStatsEnabled(serviceCtx)) {
        return false;
    }
    // Cannot collect queryStats if sampling rate is not greater than 0. Note that we do not
    // increment queryStatsRateLimitedRequestsMetric here since queryStats is entirely disabled.
    if (queryStatsRateLimiter(serviceCtx)->getSamplingRate() <= 0) {
        return false;
    }
    // Check if rate limiting allows us to collect queryStats for this request.
    if (queryStatsRateLimiter(serviceCtx)->getSamplingRate() < INT_MAX &&
        !queryStatsRateLimiter(serviceCtx)->handleRequestSlidingWindow()) {
        queryStatsRateLimitedRequestsMetric.increment();
        return false;
    }
    return true;
}

std::string sha256HmacStringDataHasher(std::string key, const StringData& sd) {
    auto hashed = SHA256Block::computeHmac(
        (const uint8_t*)key.data(), key.size(), (const uint8_t*)sd.rawData(), sd.size());
    return hashed.toString();
}

static const StringData replacementForLiteralArgs = "?"_sd;

std::size_t hash(const BSONObj& obj) {
    return absl::hash_internal::CityHash64(obj.objdata(), obj.objsize());
}

}  // namespace

BSONObj QueryStatsEntry::computeQueryStatsKey(OperationContext* opCtx,
                                              bool applyHmacToIdentifiers,
                                              std::string hmacKey) const {
    SerializationOptions options;
    options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    options.replacementForLiteralArgs = replacementForLiteralArgs;
    if (applyHmacToIdentifiers) {
        options.applyHmacToIdentifiers = true;
        options.identifierHmacPolicy = [&](StringData sd) {
            return sha256HmacStringDataHasher(hmacKey, sd);
        };
    }
    return requestShapifier->makeQueryStatsKey(options, opCtx);
}

void registerRequest(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     const NamespaceString& collection,
                     std::function<std::unique_ptr<RequestShapifier>(void)> makeShapifier) {
    auto opCtx = expCtx->opCtx;
    if (!isQueryStatsEnabled(opCtx->getServiceContext())) {
        return;
    }

    // Queries against metadata collections should never appear in queryStats data.
    if (collection.isFLE2StateCollection()) {
        return;
    }

    if (!shouldCollect(opCtx->getServiceContext())) {
        return;
    }
    SerializationOptions options;
    options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    options.replacementForLiteralArgs = replacementForLiteralArgs;
    auto& opDebug = CurOp::get(opCtx)->debug();
    opDebug.queryStatsRequestShapifier = makeShapifier();
    opDebug.queryStatsStoreKeyHash =
        hash(opDebug.queryStatsRequestShapifier->makeQueryStatsKey(options, expCtx));
}

QueryStatsStore& getQueryStatsStore(OperationContext* opCtx) {
    uassert(6579000,
            "Telemetry is not enabled without the feature flag on and a cache size greater than 0 "
            "bytes",
            isQueryStatsEnabled(opCtx->getServiceContext()));
    return queryStatsStoreDecoration(opCtx->getServiceContext())->getQueryStatsStore();
}

void writeQueryStats(OperationContext* opCtx,
                     boost::optional<size_t> queryStatsKeyHash,
                     std::unique_ptr<RequestShapifier> requestShapifier,
                     const uint64_t queryExecMicros,
                     const uint64_t docsReturned) {
    if (!queryStatsKeyHash) {
        return;
    }
    auto&& queryStatsStore = getQueryStatsStore(opCtx);
    auto&& [statusWithMetrics, partitionLock] =
        queryStatsStore.getWithPartitionLock(*queryStatsKeyHash);
    std::shared_ptr<QueryStatsEntry> metrics;
    if (statusWithMetrics.isOK()) {
        metrics = *statusWithMetrics.getValue();
    } else {
        tassert(7315200,
                "requestShapifier cannot be null when writing a new entry to the telemetry store",
                requestShapifier != nullptr);
        size_t numEvicted =
            queryStatsStore.put(*queryStatsKeyHash,
                                std::make_shared<QueryStatsEntry>(std::move(requestShapifier),
                                                                  CurOp::get(opCtx)->getNSS()),
                                partitionLock);
        queryStatsEvictedMetric.increment(numEvicted);
        auto newMetrics = partitionLock->get(*queryStatsKeyHash);
        if (!newMetrics.isOK()) {
            // This can happen if the budget is immediately exceeded. Specifically if the there is
            // not enough room for a single new entry if the number of partitions is too high
            // relative to the size.
            queryStatsStoreWriteErrorsMetric.increment();
            LOGV2_DEBUG(7560900,
                        1,
                        "Failed to store queryStats entry.",
                        "status"_attr = newMetrics.getStatus(),
                        "queryStatsKeyHash"_attr = queryStatsKeyHash);
            return;
        }
        metrics = newMetrics.getValue()->second;
    }

    metrics->lastExecutionMicros = queryExecMicros;
    metrics->execCount++;
    metrics->queryExecMicros.aggregate(queryExecMicros);
    metrics->docsReturned.aggregate(docsReturned);
}
}  // namespace query_stats
}  // namespace mongo
