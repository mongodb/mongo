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

#include <memory>

namespace mongo::otel::traces {
namespace {
/** Production implementation of the TracingSampler interface. */
class TracingSamplerImpl : public TracingSampler {
public:
    bool shouldSample(StringData) const override {
        // TODO(SERVER-127463): Use server parameters and default sampling to determine this.
        return true;
    }
};

class FunctionSampler : public TracingSampler {
public:
    explicit FunctionSampler(unique_function<bool(StringData)> fn) : _fn(std::move(fn)) {}
    bool shouldSample(StringData spanName) const override {
        return _fn(spanName);
    }

private:
    unique_function<bool(StringData)> _fn;
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

ScopedSamplerOverride setTraceSamplingFnForTest(unique_function<bool(StringData)> fn) {
    return std::make_unique<SamplerOverrideImpl>(std::make_unique<FunctionSampler>(std::move(fn)));
}

TracingSampler& TracingSampler::get() {
    return *globalSampler();
}

}  // namespace mongo::otel::traces
