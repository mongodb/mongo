// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameter.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::otel::traces {

/**
 * Sets a server parameter for its lifetime and restores the previous value afterwards. This is
 * benchmark-only infrastructure — no production code should set server parameters this way.
 *
 * Unlike unittest::ServerParameterGuard, this has no dependency on a test fixture, so it can be
 * used from benchmarks and other non-test binaries, including ones (like mongos) that don't link
 * mongod-only test fixtures.
 */
class [[MONGO_MOD_PUBLIC]] ScopedServerParameter {
public:
    ScopedServerParameter(std::string_view name, std::string_view value)
        : _param(ServerParameterSet::getNodeParameterSet()->get(name)) {
        BSONObjBuilder bob;
        _param->append(nullptr, &bob, name);
        _previous = bob.obj();
        uassertStatusOK(_param->setFromString(value));
    }

    ~ScopedServerParameter() {
        uassertStatusOK(_param->set(_previous.firstElement()));
    }

    ScopedServerParameter(const ScopedServerParameter&) = delete;
    ScopedServerParameter& operator=(const ScopedServerParameter&) = delete;

private:
    ServerParameter* _param;
    BSONObj _previous;
};

/**
 * The smallest positive sampling factor representable as a double. A benchmark fixture that wants
 * to measure the cost of a sampling-miss decision (as opposed to a sampling-off span name that
 * isn't in the sampler's map at all) uses this instead of 0.0, so that a span name is registered
 * and the sampler's roll-and-compare logic actually runs, while making a sampling hit (and the
 * resulting contamination of the measurement by the rarer, more expensive hit path) astronomically
 * unlikely: a hit can only occur if the sampling roll is exactly 0.0.
 */
inline constexpr double kSmallestSamplingFactor = std::numeric_limits<double>::denorm_min();

/**
 * Effectively unlimited, for a benchmark fixture to pass as SamplingParameters::rateLimits so that
 * rate limiting never drops a sampled span during the benchmark.
 */
inline constexpr RateLimitParams kUnlimitedRateLimit{
    .refillRate = 1.0e9, .maxTokens = std::numeric_limits<int>::max() / 2};

/**
 * RAII type that configures tracing for the duration of a benchmark. A benchmark fixture that
 * wants tracing constructs one of these (typically in an override of a hook like
 * ServiceEntryPointBenchmarkFixture::setupTracingSession()) with whatever parameters describe the
 * configuration it wants to measure; a fixture that doesn't want tracing simply doesn't construct
 * one, leaving tracing uninitialized as on a server started without an exporter.
 *
 * Spans are always exported through a real BatchSpanProcessor on a background thread with the
 * server's default batching, through the real OtlpHttpExporter mongod uses in production, pointed
 * at a local dummy collector owned by this object that accepts and discards every request: mongod
 * never exports any other way (see tracer_provider_service_factory.cpp), so running the real
 * exporter against a local collector catches regressions in the exporter's own code without
 * depending on an external service or affecting the benchmark thread's measured time, which never
 * includes export. The background thread's export CPU time is reported on stderr.
 *
 * samplingParameters.factor configures how many spans the sampler selects: 0.0 (the default)
 * samples nothing, so a benchmark measures the cost paid by untraced work, while 1.0 samples every
 * span, so a benchmark measures the cost paid by fully traced work. A factor in between, such as
 * kSmallestSamplingFactor, measures the cost of a sampling-miss decision for a span name that is
 * registered with the sampler (see kSmallestSamplingFactor). `sampledSpanNames` lists spans
 * sampled with samplingParameters in addition to those sampled by default, e.g. command spans on
 * mongos; command spans are never sampled by default on mongod, regardless of factor (see
 * Command::initializeClusterRole()).
 *
 * On destruction, a run configured to sample everything (factor >= 1.0) that exported no spans, or
 * a run configured to sample nothing (factor == 0.0) that exported any, fails an invariant, so that
 * a misconfigured benchmark cannot silently pass its thresholds. Any other factor may export a few
 * spans or none, so it is not checked.
 *
 * When OpenTelemetry is compiled out, tracing has no effect and isSupported() returns false.
 */
class [[MONGO_MOD_PUBLIC]] BenchmarkTracingSession {
public:
    explicit BenchmarkTracingSession(SamplingParameters samplingParameters = {},
                                     std::vector<std::string> sampledSpanNames = {});
    ~BenchmarkTracingSession();

    BenchmarkTracingSession(const BenchmarkTracingSession&) = delete;
    BenchmarkTracingSession& operator=(const BenchmarkTracingSession&) = delete;

    /** Returns whether tracing is compiled in, i.e. whether this class has any effect. */
    static bool isSupported();

    /** Flushes pending spans and returns how many have been exported so far. */
    int64_t exportedSpans() const;

private:
    struct State;
    std::unique_ptr<State> _state;
};

}  // namespace mongo::otel::traces
