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

#include "mongo/platform/basic.h"

#include "mongo/s/query_analysis_sampler.h"

#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/refresh_query_analyzer_configuration_cmd_gen.h"
#include "mongo/util/net/socket_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisSampler);

const auto getQueryAnalysisSampler = ServiceContext::declareDecoration<QueryAnalysisSampler>();

bool isApproximatelyEqual(double val0, double val1, double epsilon) {
    return std::fabs(val0 - val1) < (epsilon + std::numeric_limits<double>::epsilon());
}

}  // namespace

QueryAnalysisSampler& QueryAnalysisSampler::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisSampler& QueryAnalysisSampler::get(ServiceContext* serviceContext) {
    invariant(analyze_shard_key::gFeatureFlagAnalyzeShardKey.isEnabled(
                  serverGlobalParams.featureCompatibility),
              "Only support analyzing queries when the feature flag is enabled");
    invariant(isMongos(), "Only support analyzing queries on a sharded cluster");
    return getQueryAnalysisSampler(serviceContext);
}

void QueryAnalysisSampler::onStartup() {
    auto serviceContext = getQueryAnalysisSampler.owner(this);
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob queryStatsRefresherJob(
        "QueryAnalysisQueryStatsRefresher",
        [this](Client* client) { _refreshQueryStats(); },
        Seconds(1));
    _periodicQueryStatsRefresher = periodicRunner->makeJob(std::move(queryStatsRefresherJob));
    _periodicQueryStatsRefresher.start();

    PeriodicRunner::PeriodicJob configurationsRefresherJob(
        "QueryAnalysisConfigurationsRefresher",
        [this](Client* client) {
            auto opCtx = client->makeOperationContext();
            _refreshConfigurations(opCtx.get());
        },
        Seconds(gQueryAnalysisSamplerConfigurationRefreshSecs));
    _periodicConfigurationsRefresher =
        periodicRunner->makeJob(std::move(configurationsRefresherJob));
    _periodicConfigurationsRefresher.start();
}

void QueryAnalysisSampler::onShutdown() {
    if (_periodicQueryStatsRefresher.isValid()) {
        _periodicQueryStatsRefresher.stop();
    }
    if (_periodicConfigurationsRefresher.isValid()) {
        _periodicConfigurationsRefresher.stop();
    }
}

double QueryAnalysisSampler::QueryStats::_calculateExponentialMovingAverage(
    double prevAvg, long long newVal) const {
    return (1 - _smoothingFactor) * prevAvg + _smoothingFactor * newVal;
}

void QueryAnalysisSampler::QueryStats::refreshTotalCount(long long newTotalCount) {
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

    long long newTotalCount = globalOpCounters.getQuery()->load() +
        globalOpCounters.getInsert()->load() + globalOpCounters.getUpdate()->load() +
        globalOpCounters.getDelete()->load() + globalOpCounters.getCommand()->load();

    stdx::lock_guard<Latch> lk(_mutex);
    _queryStats.refreshTotalCount(newTotalCount);
}

double QueryAnalysisSampler::SampleRateLimiter::_getBurstCapacity(double numTokensPerSecond) {
    return std::max(1.0, gQueryAnalysisSamplerBurstMultiplier.load() * numTokensPerSecond);
}

void QueryAnalysisSampler::SampleRateLimiter::_refill(double numTokensPerSecond,
                                                      double burstCapacity) {
    auto now = _serviceContext->getFastClockSource()->now();
    double numSecondsElapsed =
        duration_cast<Microseconds>(now - _lastRefillTime).count() / 1000000.0;
    if (numSecondsElapsed > 0) {
        _lastNumTokens =
            std::min(burstCapacity, numSecondsElapsed * numTokensPerSecond + _lastNumTokens);
        _lastRefillTime = now;
    }
}

bool QueryAnalysisSampler::SampleRateLimiter::tryConsume() {
    _refill(_numTokensPerSecond, _getBurstCapacity(_numTokensPerSecond));

    if (_lastNumTokens >= 1) {
        _lastNumTokens -= 1;
        return true;
    } else if (isApproximatelyEqual(_lastNumTokens, 1, kEpsilon)) {
        // To avoid skipping queries that could have been sampled, allow one token to be consumed
        // if there is nearly one.
        _lastNumTokens = 0;
        return true;
    }
    return false;
}

void QueryAnalysisSampler::SampleRateLimiter::refreshRate(double numTokensPerSecond) {
    // Fill the bucket with tokens created by the previous rate before setting a new rate.
    _refill(_numTokensPerSecond, _getBurstCapacity(numTokensPerSecond));
    _numTokensPerSecond = numTokensPerSecond;
}

void QueryAnalysisSampler::_refreshConfigurations(OperationContext* opCtx) {
    if (MONGO_unlikely(disableQueryAnalysisSampler.shouldFail())) {
        return;
    }

    if (!_queryStats.getLastAvgCount()) {
        // The average number of queries executed per second has not been calculated yet.
        return;
    }

    RefreshQueryAnalyzerConfiguration cmd;
    cmd.setDbName(NamespaceString::kAdminDb);
    cmd.setName(getHostNameCached() + ":" + std::to_string(serverGlobalParams.port));
    cmd.setNumQueriesExecutedPerSecond(*_queryStats.getLastAvgCount());

    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto swResponse = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        NamespaceString::kAdminDb.toString(),
        cmd.toBSON({}),
        Shard::RetryPolicy::kIdempotent);
    auto status = Shard::CommandResponse::getEffectiveStatus(swResponse);

    if (!status.isOK()) {
        LOGV2(6973904,
              "Failed to refresh query analysis configurations, will try again at the next "
              "refresh interval",
              "error"_attr = redact(status));
        return;
    }

    auto response = RefreshQueryAnalyzerConfigurationResponse::parse(
        IDLParserContext("configurationRefresher"), swResponse.getValue().response);

    stdx::lock_guard<Latch> lk(_mutex);
    std::map<NamespaceString, SampleRateLimiter> sampleRateLimiters;
    for (const auto& configuration : response.getConfigurations()) {
        auto it = _sampleRateLimiters.find(configuration.getNs());
        if (it == _sampleRateLimiters.end() ||
            it->second.getCollectionUuid() != configuration.getCollectionUuid()) {
            // There is no existing SampleRateLimiter for the collection with this specific
            // collection uuid so create one for it.
            sampleRateLimiters.emplace(configuration.getNs(),
                                       SampleRateLimiter{opCtx->getServiceContext(),
                                                         configuration.getNs(),
                                                         configuration.getCollectionUuid(),
                                                         configuration.getSampleRate()});
        } else {
            auto rateLimiter = it->second;
            invariant(rateLimiter.getNss() == configuration.getNs());
            rateLimiter.refreshRate(configuration.getSampleRate());
            sampleRateLimiters.emplace(configuration.getNs(), std::move(rateLimiter));
        }
    }
    _sampleRateLimiters = std::move(sampleRateLimiters);
}

bool QueryAnalysisSampler::shouldSample(const NamespaceString& nss) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto it = _sampleRateLimiters.find(nss);

    if (it == _sampleRateLimiters.end()) {
        return false;
    }

    auto& rateLimiter = it->second;
    return rateLimiter.tryConsume();
}

}  // namespace analyze_shard_key
}  // namespace mongo
