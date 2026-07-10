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
