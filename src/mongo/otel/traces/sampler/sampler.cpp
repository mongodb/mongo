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

#include "mongo/util/static_immortal.h"

namespace mongo::otel::traces {

TracingSamplerImpl::TracingSamplerImpl() {
    _samplingFactors.update(std::make_shared<const SamplingFactorMap>());
}

bool TracingSamplerImpl::shouldSample(StringData spanName, double sampleValue) const {
    thread_local auto snapshot = _samplingFactors.makeSnapshot();
    _samplingFactors.refreshSnapshot(snapshot);
    auto it = snapshot->find(spanName);
    if (it == snapshot->end()) {
        return false;
    }
    return sampleValue < it->second;
}

void TracingSamplerImpl::updateConfig(const SamplingConfig& config) {
    std::lock_guard lk(_mutex);
    _samplingConfig = config;
    _rebuild(lk);
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
    auto newSamplingFactors = std::make_shared<SamplingFactorMap>();
    for (const auto& name : _defaultSampledSpanNames) {
        (*newSamplingFactors)[name] = _samplingConfig.defaultFactor;
    }
    _samplingFactors.update(std::move(newSamplingFactors));
}

namespace {

class FunctionSampler : public TracingSampler {
public:
    explicit FunctionSampler(unique_function<bool(StringData, double)> fn) : _fn(std::move(fn)) {}

    bool shouldSample(StringData spanName, double sampleValue) const override {
        return _fn(spanName, sampleValue);
    }

private:
    unique_function<bool(StringData, double)> _fn;
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

ScopedSamplerOverride setTraceSamplingFnForTest(unique_function<bool(StringData, double)> fn) {
    return std::make_unique<SamplerOverrideImpl>(std::make_unique<FunctionSampler>(std::move(fn)));
}

TracingSampler& TracingSampler::get() {
    return *globalSampler();
}

}  // namespace mongo::otel::traces
