// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::otel::traces {

inline constexpr double kDefaultFactor = 0.0;
inline constexpr double kDefaultRefillRate = 1.0;
inline constexpr int kDefaultMaxTokens = 10;

struct RateLimitParams {
    /** The rate at which tokens are added to the span's rate limiter token bucket, in tokens per
     * second. */
    double refillRate = kDefaultRefillRate;

    /** The maximum number of tokens that can be stored in the span's rate limiter token bucket. */
    int maxTokens = kDefaultMaxTokens;
};

struct SamplingParameters {
    /** Sampling factor for a span. 0.0 means never sampled, while 1.0 means always sampled. */
    double factor = kDefaultFactor;

    /** Parameters used for rate limiting spans. */
    RateLimitParams rateLimits;
};

/** Configuration for trace sampling. */
struct SamplingConfig {
    /** The parameters used for spans that are sampled by default. */
    SamplingParameters defaultSpans;

    /** The rate limiter parameters used for spans whose trace was initiated externally from this
     * server. */
    RateLimitParams externalRateLimits;

    /**
     * Per-span-name overrides. An entry here takes precedence over defaultSpans for that span name,
     * if applicable.
     */
    StringMap<SamplingParameters> perSpanOverrides;
};

}  // namespace mongo::otel::traces
