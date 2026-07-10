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

#pragma once

#include "mongo/db/admission/rate_limiter.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/otel/traces/span/span_name.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/versioned_value.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace mongo::otel::traces {
struct [[MONGO_MOD_PUBLIC]] RateLimiterStats {
    int64_t admitted = 0;
    int64_t rejected = 0;
};

/** A snapshot of the tracing sampler's admission stats. For simplicity in reporting, the stats for
 * each internal span are aggregated under `internalSpans`.
 */
struct [[MONGO_MOD_PUBLIC]] TracingSamplerStats {
    RateLimiterStats internalSpans;
    RateLimiterStats externalSpan;
};

/** Decides whether a span should initiate a trace. */
class [[MONGO_MOD_PUBLIC]] TracingSampler {
public:
    /** Returns the global sampler instance. */
    static TracingSampler& get();

    virtual ~TracingSampler() = default;

    /**
     * Returns whether the span identified by `spanName` should start a trace, given a
     * sampling roll in [0, 1].
     */
    virtual bool shouldSample(std::string_view spanName, double sampleValue) = 0;

    /** Returns whether a trace initiated externally from this server should be continued on this
     * server.
     */
    virtual bool shouldAcceptExternalTrace() const {
        return false;
    };

    /** Applies a new internal sampling configuration. */
    virtual void updateInternalConfig(const SamplingParameters& defaultSpans,
                                      const StringMap<SamplingParameters>& perSpanOverrides) {}

    /** Applies a new rate limit configuration for externally-sampled traces. */
    virtual void updateExternalConfig(RateLimitParams rateLimits) {}

    /** Returns the current sampling configuration. */
    virtual SamplingConfig getConfig() const {
        return SamplingConfig{};
    }

    /**
     * Returns a snapshot of the aggregate admission statistics.
     */
    virtual TracingSamplerStats getStats() const {
        return TracingSamplerStats{};
    }

    /**
     * Registers a span name so that it is eligible for sampling if no config specifies otherwise.
     */
    virtual void sampleByDefault(SpanName name) {}
};

/** Stores the current state of a sampler. */
struct [[MONGO_MOD_PUBLIC]] SamplerState {
    using RateLimiter = std::shared_ptr<admission::RateLimiter>;

    /** Maps span name to its sampling factor. Absence is equivalent to a factor of 0.0. */
    using SamplingFactorMap = StringMap<double>;

    /**
     * Maps span name to its rate limiter. Each limiter is a shared_ptr so that _rebuild can copy
     * existing limiters into a fresh map without resetting their token buckets.
     */
    using RateLimiterMap = StringMap<RateLimiter>;

    SamplingFactorMap samplingFactorMap;
    RateLimiterMap rateLimiterMap;
    RateLimiter externalRateLimiter;
};

/**
 * Production `TracingSampler` implementation. This is designed to be a thread-safe singleton, and
 * having multiple instances at once may not work correctly.
 */
class [[MONGO_MOD_PUBLIC]] TracingSamplerImpl : public TracingSampler {
public:
    explicit TracingSamplerImpl(TickSource* tickSource = globalSystemTickSource());

    /**
     * Determines if the span should start a trace given the combination of config, default sampled
     * spans, and sampleValue.
     */
    bool shouldSample(std::string_view spanName, double sampleValue) override;
    bool shouldAcceptExternalTrace() const override;
    void updateInternalConfig(const SamplingParameters& defaultSpans,
                              const StringMap<SamplingParameters>& perSpanOverrides) override;
    void updateExternalConfig(RateLimitParams rateLimits) override;
    SamplingConfig getConfig() const override;
    void sampleByDefault(SpanName name) override;

    /**
     * Returns a TracingSamplerStats by summing the succeeded and rejected admissions across the
     * currently-configured rate limiters.
     */
    TracingSamplerStats getStats() const override;

private:
    /** Rebuilds _samplingFactors from _defaultSampledSpanNames and _samplingConfig. */
    void _rebuild(WithLock);

    static VersionedValue<const SamplerState> _samplerState;
    static thread_local VersionedValue<const SamplerState>::Snapshot _snapshot;

    TickSource* _tickSource;

    mutable std::mutex _mutex;
    /** Span names that are sampled when there is no overriding configuration. */
    stdx::unordered_set<std::string> _defaultSampledSpanNames;
    /** The sampling configuration for spans that are default sampled. */
    SamplingConfig _samplingConfig;
};

/** Interface for overriding the global sampler, for use in tests. */
class SamplerOverride {
public:
    virtual ~SamplerOverride() = default;
};
using ScopedSamplerOverride [[MONGO_MOD_PUBLIC]] = std::unique_ptr<SamplerOverride>;

/**
 * Replaces the global sampler with one that calls fn for `shouldSample`, for testing purposes.
 * Returns a guard that restores the previous sampler on destruction. This is not thread-safe.
 */
[[nodiscard]] [[MONGO_MOD_PUBLIC]] ScopedSamplerOverride setTraceSamplingFnForTest(
    unique_function<bool(std::string_view, double)> fn);

}  // namespace mongo::otel::traces
