// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/sampler/sampler.h"

#include "mongo/db/admission/rate_limiter_otel_metrics_recorder.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/tick_source.h"

namespace mongo::otel::traces {
namespace {

using admission::RateLimiter;
using admission::RateLimiterMetricsRecorder;
using admission::RateLimiterOtelMetricsRecorder;
using otel::metrics::MetricName;
using otel::metrics::MetricNames;

// Builds rate limiter options that record into the given metrics recorder. Use this function if the
// rate limiter should own the RateLimiterMetricsRecorder.
RateLimiter::Options makeRateLimiterOptions(
    TickSource* tickSource, std::unique_ptr<RateLimiterMetricsRecorder> metricsRecorder) {
    return RateLimiter::Options{.tickSource = tickSource,
                                .metricsRecorder = std::move(metricsRecorder)};
}

// Builds rate limiter options that record into the given metrics recorder. Use this function if the
// rate limiter should NOT own the RateLimiterMetricsRecorder.
RateLimiter::Options makeRateLimiterOptions(TickSource* tickSource,
                                            RateLimiterMetricsRecorder* metricsRecorder) {
    return RateLimiter::Options{.tickSource = tickSource, .metricsRecorder = metricsRecorder};
}

// Builds a RateLimiterOtelMetricsRecorder that records only successful and rejected admissions for
// the otel metrics referred to by the given metric names.
std::unique_ptr<RateLimiterOtelMetricsRecorder> makeMetricsRecorder(MetricName successfulAdmissions,
                                                                    MetricName rejectedAdmissions) {
    return std::make_unique<RateLimiterOtelMetricsRecorder>(
        RateLimiterOtelMetricsRecorder::MetricsSpec{.successfulAdmissions = successfulAdmissions,
                                                    .rejectedAdmissions = rejectedAdmissions});
}

// Returns the process-wide metrics recorder shared by every internal span rate limiter. Sharing a
// single recorder means all internal spans record into the same instruments, so their stats are
// aggregated together across spans. It is an immortal singleton so that it outlives the rate
// limiters stored in TracingSamplerImpl::_samplerState, which reference it via raw pointers.
RateLimiterMetricsRecorder* internalMetricsRecorder() {
    static StaticImmortal<std::unique_ptr<RateLimiterMetricsRecorder>> recorder(makeMetricsRecorder(
        MetricNames::kOtelTracingSamplerInternalSpanRateLimiterSuccessfulAdmissions,
        MetricNames::kOtelTracingSamplerInternalSpanRateLimiterRejectedAdmissions));
    return recorder->get();
}
}  // namespace

VersionedValue<const SamplerState> TracingSamplerImpl::_samplerState;
thread_local VersionedValue<const SamplerState>::Snapshot TracingSamplerImpl::_snapshot =
    TracingSamplerImpl::_samplerState.makeSnapshot();

VersionedValue<const RateLimiterMap> TracingSamplerImpl::_internalRateLimiters{
    std::make_shared<const RateLimiterMap>()};
thread_local VersionedValue<const RateLimiterMap>::Snapshot TracingSamplerImpl::_rlSnapshot =
    TracingSamplerImpl::_internalRateLimiters.makeSnapshot();

TracingSamplerImpl::TracingSamplerImpl(TickSource* tickSource) : _tickSource(tickSource) {
    auto& externalRateLimitParams = _samplingConfig.externalRateLimits;
    auto externalBurstCapacitySecs =
        externalRateLimitParams.maxTokens / externalRateLimitParams.refillRate;

    _samplerState.update(std::make_shared<const SamplerState>(SamplerState::SamplingParamsMap{}));

    _internalRateLimiters.update(std::make_shared<const RateLimiterMap>());

    _externalRateLimiter = std::make_unique<RateLimiter>(
        externalRateLimitParams.refillRate,
        externalBurstCapacitySecs,
        0 /* maxQueueDepth */,
        "external",
        makeRateLimiterOptions(
            tickSource,
            makeMetricsRecorder(
                MetricNames::kOtelTracingSamplerExternalSpanRateLimiterSuccessfulAdmissions,
                MetricNames::kOtelTracingSamplerExternalSpanRateLimiterRejectedAdmissions)));
}

bool TracingSamplerImpl::shouldSample(std::string_view spanName, double sampleValue) {
    _samplerState.refreshSnapshot(_snapshot);
    auto it = _snapshot->samplingParamsMap.find(spanName);
    if (it == _snapshot->samplingParamsMap.end()) {
        return false;
    }
    const auto& params = it->second;

    if (sampleValue >= params.factor) {
        return false;
    }

    auto rateLimiter = _getOrCreateRateLimiter(spanName, params.rateLimits, _snapshot.version());
    return rateLimiter->tryAcquireToken(1);
}

bool TracingSamplerImpl::shouldAcceptExternalTrace() const {
    return _externalRateLimiter->tryAcquireToken(1);
}

void TracingSamplerImpl::updateInternalConfig(
    const SamplingParameters& defaultSpans, const StringMap<SamplingParameters>& perSpanOverrides) {
    std::lock_guard lk(_mutex);
    _samplingConfig.defaultSpans = defaultSpans;
    _samplingConfig.perSpanOverrides = perSpanOverrides;
    _rebuild(lk);
}

void TracingSamplerImpl::updateExternalConfig(RateLimitParams rateLimits) {
    invariant(rateLimits.refillRate > 0);
    invariant(rateLimits.maxTokens > 0);
    std::lock_guard lk(_mutex);
    _samplingConfig.externalRateLimits = rateLimits;
    auto burstCapacitySecs = rateLimits.maxTokens / rateLimits.refillRate;
    invariant(_externalRateLimiter);
    _externalRateLimiter->updateRateParameters(rateLimits.refillRate, burstCapacitySecs);
}

SamplingConfig TracingSamplerImpl::getConfig() const {
    std::lock_guard lk(_mutex);
    return _samplingConfig;
}

TracingSamplerStats TracingSamplerImpl::getStats() const {
    TracingSamplerStats stats;
    stats.internalSpans.admitted = internalMetricsRecorder()->successfulAdmissions();
    stats.internalSpans.rejected = internalMetricsRecorder()->rejectedAdmissions();

    const auto& externalStats = _externalRateLimiter->stats();
    stats.externalSpan.admitted = externalStats.successfulAdmissions();
    stats.externalSpan.rejected = externalStats.rejectedAdmissions();

    return stats;
}

size_t TracingSamplerImpl::getNumInternalRateLimiters() const {
    return _internalRateLimiters.makeSnapshot()->size();
}

void TracingSamplerImpl::sampleByDefault(SpanName name) {
    std::lock_guard lk(_mutex);
    if (_defaultSampledSpanNames.emplace(name.getName()).second) {
        _rebuild(lk);
    }
}

void TracingSamplerImpl::_rebuild(WithLock) {
    SamplerState::SamplingParamsMap newSamplingParams;

    auto addSpanSamplingParams = [&](std::string_view name, const SamplingParameters& params) {
        invariant(params.rateLimits.refillRate > 0,
                  fmt::format("Invalid refillRate for {} span", name));
        invariant(params.rateLimits.maxTokens > 0,
                  fmt::format("Invalid maxTokens for {} span", name));
        newSamplingParams[name] = params;
    };

    for (const auto& name : _defaultSampledSpanNames) {
        addSpanSamplingParams(name, _samplingConfig.defaultSpans);
    }
    // Per-span overrides intentionally overwrite the default-seeded entries, and also apply to
    // spans that were never registered via sampleByDefault.
    for (const auto& [name, params] : _samplingConfig.perSpanOverrides) {
        addSpanSamplingParams(name, params);
    }

    // update() bumps the version, so existing limiters re-parameterize on their next hit.
    _samplerState.update(std::make_shared<const SamplerState>(std::move(newSamplingParams)));
}

std::shared_ptr<admission::RateLimiter> TracingSamplerImpl::_getOrCreateRateLimiter(
    std::string_view name, const RateLimitParams& rateLimits, uint64_t generation) {
    // If a limiter already exists and reflects the current config generation, re-use as is.
    _internalRateLimiters.refreshSnapshot(_rlSnapshot);
    if (auto it = _rlSnapshot->find(name);
        it != _rlSnapshot->end() && it->second.generation == generation) {
        return it->second.limiter;
    }

    // If this is the first sighting of this span or the config has changed since the last
    // generation, update existing or create new limiter. Rate-limit params were validated at config
    // time in _rebuild.
    double refillRate = rateLimits.refillRate;
    int maxTokens = rateLimits.maxTokens;
    double burstCapacitySecs = maxTokens / refillRate;

    std::lock_guard lk(_mutex);

    auto oldMap = _internalRateLimiters.makeSnapshot();
    auto newMap = std::make_shared<RateLimiterMap>(*oldMap);

    auto& entry = (*newMap)[std::string(name)];
    if (entry.limiter) {
        entry.limiter->updateRateParameters(refillRate, burstCapacitySecs);
    } else {
        entry.limiter = std::make_shared<RateLimiter>(
            refillRate,
            burstCapacitySecs,
            0 /* maxQueueDepth */,
            std::string(name),
            // TODO(SERVER-131083): Once the rate limiter otel stats have span name attributes,
            // each recorder should write to its attribute rather than the aggregated total.
            makeRateLimiterOptions(_tickSource, internalMetricsRecorder()));
    }
    entry.generation = generation;
    auto limiter = entry.limiter;
    _internalRateLimiters.update(std::move(newMap));
    return limiter;
}

namespace {

class FunctionSampler : public TracingSampler {
public:
    explicit FunctionSampler(
        unique_function<bool(std::string_view, double)> shouldSample,
        unique_function<bool()> shouldAcceptExternalTrace = [] { return false; })
        : _shouldSample(std::move(shouldSample)),
          _shouldAcceptExternalTrace(std::move(shouldAcceptExternalTrace)) {}

    bool shouldSample(std::string_view spanName, double sampleValue) override {
        return _shouldSample(spanName, sampleValue);
    }

    bool shouldAcceptExternalTrace() const override {
        return _shouldAcceptExternalTrace();
    }

private:
    unique_function<bool(std::string_view, double)> _shouldSample;
    unique_function<bool()> _shouldAcceptExternalTrace;
};

std::unique_ptr<TracingSampler>& globalSampler() {
    static StaticImmortal<std::unique_ptr<TracingSampler>> instance(
        std::make_unique<TracingSamplerImpl>());
    return *instance;
}

}  // namespace

/**
 * RAII guard that restores the previous global sampler on destruction.
 */
class SamplerOverrideImpl : public SamplerOverride {
public:
    explicit SamplerOverrideImpl(std::unique_ptr<TracingSampler> newSampler)
        : _previous(std::move(globalSampler())) {
        globalSampler() = std::move(newSampler);
    }

    ~SamplerOverrideImpl() override {
        invariant(_previous != nullptr);
        globalSampler() = std::move(_previous);
    }

private:
    std::unique_ptr<TracingSampler> _previous;
};

ScopedSamplerOverride setTraceSamplingFnForTest(
    unique_function<bool(std::string_view, double)> shouldSample,
    unique_function<bool()> shouldAcceptExternalTrace) {
    return std::make_unique<SamplerOverrideImpl>(std::make_unique<FunctionSampler>(
        std::move(shouldSample), std::move(shouldAcceptExternalTrace)));
}

TracingSampler& TracingSampler::get() {
    return *globalSampler();
}

}  // namespace mongo::otel::traces
