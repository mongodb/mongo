// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/span/span_telemetry_context_impl.h"

#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"

#include <opentelemetry/trace/span_context.h>

namespace mongo {
namespace otel {
namespace traces {

namespace {
PseudoRandom& defaultPrng() {
    thread_local PseudoRandom prng{SecureRandom{}.nextInt64()};
    return prng;
}
}  // namespace

SpanTelemetryContextImpl::SpanTelemetryContextImpl() : _prng(&defaultPrng()) {}

SpanTelemetryContextImpl::SpanTelemetryContextImpl(OtelContext ctx, PseudoRandom* prng)
    : _ctx(std::move(ctx)), _prng(prng ? prng : &defaultPrng()) {}

double SpanTelemetryContextImpl::getSamplingValue() {
    if (!_samplingRoll) {
        invariant(_prng != nullptr);
        _samplingRoll = _prng->nextCanonicalDouble();
    }
    return *_samplingRoll;
}

void SpanTelemetryContextImpl::propagate(TextMapPropagator& propagator,
                                         TextMapCarrier& carrier) const {
    propagator.Inject(carrier, _ctx);
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo
