/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/traces/sampler/sampler.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/tick_source.h"

namespace mongo::otel::traces {

VersionedValue<const SamplerState> TracingSamplerImpl::_samplerState;
thread_local VersionedValue<const SamplerState>::Snapshot TracingSamplerImpl::_snapshot =
    TracingSamplerImpl::_samplerState.makeSnapshot();

using SamplingFactorMap = SamplerState::SamplingFactorMap;
using RateLimiterMap = SamplerState::RateLimiterMap;

TracingSamplerImpl::TracingSamplerImpl(TickSource* tickSource) : _tickSource(tickSource) {
    auto& externalRateLimitParams = _samplingConfig.externalRateLimits;
    auto externalBurstCapacitySecs =
        externalRateLimitParams.maxTokens / externalRateLimitParams.refillRate;

    _samplerState.update(std::make_shared<const SamplerState>(
        SamplerState::SamplingFactorMap{},
        SamplerState::RateLimiterMap{},
        std::make_shared<admission::RateLimiter>(externalRateLimitParams.refillRate,
                                                 externalBurstCapacitySecs,
                                                 0 /* maxQueueDepth */,
                                                 "external",
                                                 tickSource)));
}

bool TracingSamplerImpl::shouldSample(std::string_view spanName, double sampleValue) {
    _samplerState.refreshSnapshot(_snapshot);
    auto samplingFactor = _snapshot->samplingFactorMap.find(spanName);
    if (samplingFactor == _snapshot->samplingFactorMap.end()) {
        return false;
    }

    if (sampleValue >= samplingFactor->second) {
        return false;
    }

    auto rateLimiter = _snapshot->rateLimiterMap.find(spanName);
    if (rateLimiter == _snapshot->rateLimiterMap.end()) {
        return false;
    }

    return rateLimiter->second->tryAcquireToken(1);
}

bool TracingSamplerImpl::shouldAcceptExternalTrace() const {
    _samplerState.refreshSnapshot(_snapshot);
    return _snapshot->externalRateLimiter->tryAcquireToken(1);
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
    auto currentSnapshot = _samplerState.makeSnapshot();
    invariant(currentSnapshot->externalRateLimiter);
    currentSnapshot->externalRateLimiter->updateRateParameters(rateLimits.refillRate,
                                                               burstCapacitySecs);
}

SamplingConfig TracingSamplerImpl::getConfig() const {
    std::lock_guard lk(_mutex);
    return _samplingConfig;
}

void TracingSamplerImpl::sampleByDefault(SpanName name) {
    std::lock_guard lk(_mutex);
    if (_defaultSampledSpanNames.emplace(name.getName()).second) {
        _rebuild(lk);
    }
}

void TracingSamplerImpl::_rebuild(WithLock) {
    SamplingFactorMap newSamplingFactors;
    RateLimiterMap newRateLimiters;
    auto oldSnapshot = _samplerState.makeSnapshot();

    auto setSamplingFactorAndRateLimits = [&](std::string_view name, SamplingParameters params) {
        double refillRate = params.rateLimits.refillRate;
        int maxTokens = params.rateLimits.maxTokens;

        newSamplingFactors[name] = params.factor;

        invariant(refillRate > 0, fmt::format("Invalid refillRate for {} span", name));
        invariant(maxTokens > 0, fmt::format("Invalid maxTokens for {} span", name));
        double burstCapacitySecs = maxTokens / refillRate;
        if (auto it = oldSnapshot->rateLimiterMap.find(name);
            it != oldSnapshot->rateLimiterMap.end()) {
            // Update the existing rate limiter and copy it into the new map.
            it->second->updateRateParameters(refillRate, burstCapacitySecs);
            newRateLimiters[name] = it->second;
        } else {
            // Rate limiter didn't exist so create a new one with the correct parameters.
            newRateLimiters[name] = std::make_shared<admission::RateLimiter>(refillRate,
                                                                             burstCapacitySecs,
                                                                             0 /* maxQueueDepth */,
                                                                             std::string(name),
                                                                             _tickSource);
        }
    };

    for (const auto& name : _defaultSampledSpanNames) {
        setSamplingFactorAndRateLimits(name, _samplingConfig.defaultSpans);
    }

    // Per-span overrides intentionally overwrite the default-seeded entries above, and also
    // apply to spans that were never registered via sampleByDefault.
    for (const auto& [name, params] : _samplingConfig.perSpanOverrides) {
        setSamplingFactorAndRateLimits(name, params);
    }

    auto externalRateLimiter = oldSnapshot->externalRateLimiter;
    invariant(externalRateLimiter);
    _samplerState.update(std::make_shared<const SamplerState>(
        std::move(newSamplingFactors), std::move(newRateLimiters), std::move(externalRateLimiter)));
}

namespace {

class FunctionSampler : public TracingSampler {
public:
    explicit FunctionSampler(unique_function<bool(std::string_view, double)> fn)
        : _fn(std::move(fn)) {}

    bool shouldSample(std::string_view spanName, double sampleValue) override {
        return _fn(spanName, sampleValue);
    }

private:
    unique_function<bool(std::string_view, double)> _fn;
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
    unique_function<bool(std::string_view, double)> fn) {
    return std::make_unique<SamplerOverrideImpl>(std::make_unique<FunctionSampler>(std::move(fn)));
}

TracingSampler& TracingSampler::get() {
    return *globalSampler();
}

}  // namespace mongo::otel::traces
