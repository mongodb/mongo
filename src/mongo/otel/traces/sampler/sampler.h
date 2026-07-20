// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
     * Registers a span name as a root span eligible for the default sampling config.
     *
     * Only a small number of spans should be registered this way — typically the outermost entry
     * points to operations we want to gather traces for by default. Child spans within a sampled
     * trace are captured automatically regardless.
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

    /** Returns a TracingSamplerStats reporting the succeeded and rejected admissions. */
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
 * Replaces the global sampler with one that calls the provided functions for `shouldSample`
 * and `shouldAcceptExternalTrace`, for testing purposes.
 * Returns a guard that restores the previous sampler on destruction. This is not thread-safe.
 */
[[nodiscard]] [[MONGO_MOD_PUBLIC]] ScopedSamplerOverride setTraceSamplingFnForTest(
    unique_function<bool(std::string_view, double)> shouldSample,
    unique_function<bool()> shouldAcceptExternalTrace = [] { return false; });

}  // namespace mongo::otel::traces
