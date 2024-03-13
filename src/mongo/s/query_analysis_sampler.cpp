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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/database_name.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query_analysis_client.h"
#include "mongo/s/query_analysis_sample_tracker.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/s/refresh_query_analyzer_configuration_cmd_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

using QuerySamplingOptions = OperationContext::QuerySamplingOptions;
using ConfigurationRefreshSecs =
    decltype(QueryAnalysisSampler::observeQueryAnalysisSamplerConfigurationRefreshSecs)::Argument;

MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisSampler);
MONGO_FAIL_POINT_DEFINE(overwriteQueryAnalysisSamplerAvgLastCountToZero);
MONGO_FAIL_POINT_DEFINE(queryAnalysisSamplerFilterByComment);

const auto getQueryAnalysisSampler = ServiceContext::declareDecoration<QueryAnalysisSampler>();

constexpr auto kActiveCollectionsFieldName = "activeCollections"_sd;

bool isApproximatelyEqual(double val0, double val1, double epsilon) {
    return std::fabs(val0 - val1) < (epsilon + std::numeric_limits<double>::epsilon());
}

/**
 * Runs a _refreshQueryAnalyzerConfigurations command and returns the configurations returned by
 * the command.
 */
StatusWith<std::vector<CollectionQueryAnalyzerConfiguration>> executeRefreshCommand(
    OperationContext* opCtx, double lastAvgCount) {
    RefreshQueryAnalyzerConfiguration cmd;
    cmd.setDbName(DatabaseName::kAdmin);
    cmd.setName(getHostNameCached() + ":" + std::to_string(serverGlobalParams.port));
    cmd.setNumQueriesExecutedPerSecond(lastAvgCount);

    BSONObj resObj;
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) ||
        serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto swResponse = configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            cmd.toBSON({}),
            Shard::RetryPolicy::kIdempotent);
        if (auto status = Shard::CommandResponse::getEffectiveStatus(swResponse); !status.isOK()) {
            return status;
        }
        resObj = swResponse.getValue().response;
    } else if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        resObj = QueryAnalysisClient::get(opCtx).executeCommandOnPrimary(
            opCtx, DatabaseName::kAdmin, cmd.toBSON({}), [&](const BSONObj& resObj) {});
        if (auto status = getStatusFromCommandResult(resObj); !status.isOK()) {
            return status;
        }
    } else {
        MONGO_UNREACHABLE;
    }

    auto response = RefreshQueryAnalyzerConfigurationResponse::parse(
        IDLParserContext("configurationRefresher"), resObj);
    return response.getConfigurations();
}

}  // namespace

QueryAnalysisSampler& QueryAnalysisSampler::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisSampler& QueryAnalysisSampler::get(ServiceContext* serviceContext) {
    invariant(analyze_shard_key::supportsSamplingQueries(serviceContext));
    return getQueryAnalysisSampler(serviceContext);
}

void QueryAnalysisSampler::onStartup() {
    auto serviceContext = getQueryAnalysisSampler.owner(this);
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    stdx::lock_guard<Latch> lk(_sampleRateLimitersMutex);

    // setting isKillableByStepdown to false as _freshQueryStats has no OperationContext.
    // Holds only _queryStatsMutex and no other resources, updates only in memory states and
    // is gauranteed to complete. Can therefore trivially set isKillableByStepdown to false
    PeriodicRunner::PeriodicJob queryStatsRefresherJob(
        "QueryAnalysisQueryStatsRefresher",
        [this](Client* client) { _refreshQueryStats(); },
        Seconds(1),
        false /*isKillableByStepdown*/);
    _periodicQueryStatsRefresher = periodicRunner->makeJob(std::move(queryStatsRefresherJob));
    _periodicQueryStatsRefresher.start();

    PeriodicRunner::PeriodicJob configurationsRefresherJob(
        "QueryAnalysisConfigurationsRefresher",
        [this](Client* client) {
            auto opCtx = client->makeOperationContext();
            try {
                _refreshConfigurations(opCtx.get());
            } catch (DBException& ex) {
                LOGV2_WARNING(
                    7012500,
                    "Failed to refresh query analysis configurations, will try again at the next "
                    "interval",
                    "error"_attr = redact(ex));
            }
        },
        Seconds(gQueryAnalysisSamplerConfigurationRefreshSecs.load()),
        true /*isKillableByStepdown*/);
    _periodicConfigurationsRefresher = std::make_shared<PeriodicJobAnchor>(
        periodicRunner->makeJob(std::move(configurationsRefresherJob)));
    _periodicConfigurationsRefresher->start();

    QueryAnalysisSampler::observeQueryAnalysisSamplerConfigurationRefreshSecs.addObserver(
        [refresher = _periodicConfigurationsRefresher](const ConfigurationRefreshSecs& secs) {
            try {
                refresher->setPeriod(Seconds(secs));
            } catch (const DBException& ex) {
                LOGV2(7891301,
                      "Failed to update the period of the thread for refreshing query sampling "
                      "configurations",
                      "error"_attr = ex.toStatus());
            }
        });
}

void QueryAnalysisSampler::onShutdown() {
    if (_periodicQueryStatsRefresher.isValid()) {
        _periodicQueryStatsRefresher.stop();
    }
    if (_periodicConfigurationsRefresher && _periodicConfigurationsRefresher->isValid()) {
        _periodicConfigurationsRefresher->stop();
    }
}

void QueryAnalysisSampler::QueryStats::gotCommand(StringData cmdName) {
    if (cmdName == "findAndModify" || cmdName == "findandmodify") {
        _lastFindAndModifyQueriesCount++;
    } else if (cmdName == "aggregate") {
        _lastAggregateQueriesCount++;
    } else if (cmdName == "count") {
        _lastCountQueriesCount++;
    } else if (cmdName == "distinct") {
        _lastDistinctQueriesCount++;
    }
}

double QueryAnalysisSampler::QueryStats::_calculateExponentialMovingAverage(
    double prevAvg, long long newVal) const {
    auto smoothingFactor = gQueryAnalysisQueryStatsSmoothingFactor.load();
    return (1 - smoothingFactor) * prevAvg + smoothingFactor * newVal;
}

void QueryAnalysisSampler::QueryStats::refreshTotalCount() {
    long long newTotalCount = [&] {
        if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            return globalOpCounters.getUpdate()->load() + globalOpCounters.getDelete()->load() +
                _lastFindAndModifyQueriesCount + globalOpCounters.getQuery()->load() +
                _lastAggregateQueriesCount + _lastCountQueriesCount + _lastDistinctQueriesCount;
        } else if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            return globalOpCounters.getNestedAggregate()->load();
        }
        MONGO_UNREACHABLE;
    }();

    invariant(newTotalCount >= _lastTotalCount, "Total number of queries cannot decrease");
    long long newCount = newTotalCount - _lastTotalCount;
    // The average is only calculated after the initial count is known.
    _lastAvgCount =
        _lastAvgCount ? _calculateExponentialMovingAverage(*_lastAvgCount, newCount) : newCount;
    _lastTotalCount = newTotalCount;
}

void QueryAnalysisSampler::_refreshQueryStats() {
    if (MONGO_unlikely(disableQueryAnalysisSampler.shouldFail())) {
        return;
    }

    stdx::lock_guard<Latch> lk(_queryStatsMutex);
    _queryStats.refreshTotalCount();
}

double QueryAnalysisSampler::SampleRateLimiter::_getBurstCapacity(double numTokensPerSecond) {
    return std::max(1.0, gQueryAnalysisSamplerBurstMultiplier.load() * numTokensPerSecond);
}

void QueryAnalysisSampler::SampleRateLimiter::_refill(double numTokensPerSecond,
                                                      double burstCapacity) {
    if (numTokensPerSecond == 0.0) {
        return;
    }

    auto currTicks = _serviceContext->getTickSource()->getTicks();
    double numSecondsElapsed = _serviceContext->getTickSource()
                                   ->ticksTo<Nanoseconds>(currTicks - _lastRefillTimeTicks)
                                   .count() /
        1.0e9;
    if (numSecondsElapsed > 0) {
        _lastNumTokens =
            std::min(burstCapacity, numSecondsElapsed * numTokensPerSecond + _lastNumTokens);
        _lastRefillTimeTicks = currTicks;

        LOGV2_DEBUG(7372303,
                    3,
                    "Refilled the bucket",
                    logAttrs(_nss),
                    "collectionUUID"_attr = _collUuid,
                    "numSecondsElapsed"_attr = numSecondsElapsed,
                    "numTokensPerSecond"_attr = numTokensPerSecond,
                    "burstCapacity"_attr = burstCapacity,
                    "lastNumTokens"_attr = _lastNumTokens,
                    "lastRefillTimeTicks"_attr = _lastRefillTimeTicks);
    }
}

bool QueryAnalysisSampler::SampleRateLimiter::tryConsume() {
    _refill(_numTokensPerSecond, _getBurstCapacity(_numTokensPerSecond));

    if (_lastNumTokens >= 1) {
        _lastNumTokens -= 1;
        LOGV2_DEBUG(7372304,
                    3,
                    "Successfully consumed one token",
                    logAttrs(_nss),
                    "collectionUUID"_attr = _collUuid,
                    "lastNumTokens"_attr = _lastNumTokens);
        return true;
    } else if (isApproximatelyEqual(_lastNumTokens, 1, kEpsilon)) {
        // To avoid skipping queries that could have been sampled, allow one token to be consumed
        // if there is nearly one.
        _lastNumTokens = 0;
        LOGV2_DEBUG(7372305,
                    3,
                    "Successfully consumed approximately one token",
                    logAttrs(_nss),
                    "collectionUUID"_attr = _collUuid,
                    "lastNumTokens"_attr = _lastNumTokens);
        return true;
    }
    LOGV2_DEBUG(7372306,
                3,
                "Failed to consume one token",
                logAttrs(_nss),
                "collectionUUID"_attr = _collUuid,
                "lastNumTokens"_attr = _lastNumTokens);
    return false;
}

void QueryAnalysisSampler::SampleRateLimiter::refreshSamplesPerSecond(double numTokensPerSecond) {
    // Fill the bucket with tokens created by the previous rate before setting a new rate.
    _refill(_numTokensPerSecond, _getBurstCapacity(numTokensPerSecond));
    _numTokensPerSecond = numTokensPerSecond;
}

void QueryAnalysisSampler::_refreshConfigurations(OperationContext* opCtx) {
    if (MONGO_unlikely(disableQueryAnalysisSampler.shouldFail())) {
        return;
    }

    boost::optional<double> lastAvgCount;
    {
        stdx::lock_guard<Latch> lk(_queryStatsMutex);
        lastAvgCount = (MONGO_unlikely(overwriteQueryAnalysisSamplerAvgLastCountToZero.shouldFail())
                            ? 0
                            : _queryStats.getLastAvgCount());
    }

    if (!lastAvgCount) {
        // The average number of queries executed per second has not been calculated yet.
        return;
    }

    auto swConfigurations = executeRefreshCommand(opCtx, *lastAvgCount);

    if (!swConfigurations.isOK()) {
        LOGV2(6973904,
              "Failed to refresh query analysis configurations, will try again at the next "
              "refresh interval",
              "error"_attr = redact(swConfigurations.getStatus()));
        return;
    }

    auto configurations = swConfigurations.getValue();
    LOGV2_DEBUG(6876103,
                3,
                "Refreshed query analyzer configurations",
                "numQueriesExecutedPerSecond"_attr = lastAvgCount,
                "configurations"_attr = configurations);

    stdx::lock_guard<Latch> lk(_sampleRateLimitersMutex);

    if (configurations.size() != _sampleRateLimiters.size()) {
        LOGV2(7362407,
              "Refreshed query analyzer configurations. The number of collections with active "
              "sampling has changed.",
              "before"_attr = _sampleRateLimiters.size(),
              "after"_attr = configurations.size(),
              "configurations"_attr = configurations);
    }

    std::map<NamespaceString, SampleRateLimiter> sampleRateLimiters;
    std::array<uint64_t, srlBloomFilterNumBlocks> srlBloomFilter{};
    for (const auto& configuration : configurations) {
        auto nss = configuration.getNs();

        // Set the bit corresponding to nss's hash to 1 in 'srlBloomFilter'.
        size_t nssHash = absl::Hash<NamespaceString>{}(nss);
        size_t blockIdx = (nssHash / srlBloomFilterNumBitsPerBlock) % srlBloomFilterNumBlocks;
        size_t bit = nssHash % srlBloomFilterNumBitsPerBlock;
        srlBloomFilter[blockIdx] |= (1ull << bit);

        // Create a SampleRateLimiter or copy an existing one ('rateLimiter') for 'nss'.
        auto rateLimiter = [&] {
            auto it = _sampleRateLimiters.find(nss);
            if (it == _sampleRateLimiters.end() ||
                it->second.getCollectionUuid() != configuration.getCollectionUuid()) {
                // There is no existing SampleRateLimiter for the collection with this specific
                // collection uuid so create one for it.
                return SampleRateLimiter{opCtx->getServiceContext(),
                                         configuration.getNs(),
                                         configuration.getCollectionUuid(),
                                         configuration.getSamplesPerSecond()};
            } else {
                auto rateLimiter = it->second;
                rateLimiter.refreshSamplesPerSecond(configuration.getSamplesPerSecond());
                return rateLimiter;
            }
        }();

        // Add 'std::pair(nss, rateLimiter)' to the 'sampleRateLimiters' map.
        sampleRateLimiters.emplace(std::move(nss), std::move(rateLimiter));
    }

    // Update '_sampleRateLimiters'.
    _sampleRateLimiters = std::move(sampleRateLimiters);

    // Update '__srlBloomFilter'.
    for (size_t i = 0; i < srlBloomFilterNumBlocks; ++i) {
        _srlBloomFilter[i].store(srlBloomFilter[i]);
    }

    QueryAnalysisSampleTracker::get(opCtx).refreshConfigurations(configurations);
}

void QueryAnalysisSampler::_incrementCounters(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const SampledCommandNameEnum cmdName) {
    switch (cmdName) {
        case SampledCommandNameEnum::kFind:
        case SampledCommandNameEnum::kAggregate:
        case SampledCommandNameEnum::kCount:
        case SampledCommandNameEnum::kDistinct:
            QueryAnalysisSampleTracker::get(opCtx).incrementReads(opCtx, nss);
            break;
        case SampledCommandNameEnum::kUpdate:
        case SampledCommandNameEnum::kDelete:
        case SampledCommandNameEnum::kFindAndModify:
        case SampledCommandNameEnum::kBulkWrite:
            QueryAnalysisSampleTracker::get(opCtx).incrementWrites(opCtx, nss);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

boost::optional<UUID> QueryAnalysisSampler::tryGenerateSampleId(OperationContext* opCtx,
                                                                const NamespaceString& nss,
                                                                SampledCommandNameEnum cmdName) {
    auto opts = opCtx->getQuerySamplingOptions();

    if (!opCtx->getClient()->session() && opts != QuerySamplingOptions::kOptIn) {
        // Do not generate a sample id for an internal query unless it has explicitly opted into
        // query sampling.
        return boost::none;
    }
    if (opts == QuerySamplingOptions::kOptOut) {
        // Do not generate a sample id for a query that has explicitly opted out of query sampling.
        return boost::none;
    }

    if (cmdName == SampledCommandNameEnum::kInsert) {
        // Insert queries are not sampled by design.
        return boost::none;
    }

    if (auto scoped = queryAnalysisSamplerFilterByComment.scopedIf([&](const BSONObj& data) {
            return !opCtx->getComment() ||
                (data.getStringField("comment") != opCtx->getComment()->checkAndGetStringData());
        });
        MONGO_unlikely(scoped.isActive())) {
        return boost::none;
    }

    // Before checking '_sampleRateLimiters', check '_srlBloomFilter' first. If the bit
    // corresponding to nss's hash is 0, then we don't need to bother with acquiring
    // '_sampleRateLimitersMutex' and we can return 'boost::none'.
    size_t nssHash = absl::Hash<NamespaceString>{}(nss);
    size_t blockIdx = (nssHash / srlBloomFilterNumBitsPerBlock) % srlBloomFilterNumBlocks;
    size_t bit = nssHash % srlBloomFilterNumBitsPerBlock;
    if (((_srlBloomFilter[blockIdx].load() >> bit) & 1u) == 0u) {
        return boost::none;
    }

    stdx::lock_guard<Latch> lk(_sampleRateLimitersMutex);
    auto it = _sampleRateLimiters.find(nss);

    if (it == _sampleRateLimiters.end()) {
        return boost::none;
    }

    auto& rateLimiter = it->second;
    if (rateLimiter.tryConsume()) {
        if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            // On a standalone replica set, sample selection is done by the mongod persisting the
            // sample itself. To avoid double counting a sample, the counters will be incremented
            // by the QueryAnalysisWriter when the sample gets added to the buffer.
            _incrementCounters(opCtx, nss, cmdName);
        }
        return UUID::gen();
    }
    return boost::none;
}

}  // namespace analyze_shard_key
}  // namespace mongo
