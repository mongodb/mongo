// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/span/span_telemetry_context_impl.h"

#include "mongo/platform/random.h"

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

SpanTelemetryContextImpl::SpanTelemetryContextImpl()
    : _samplingRoll(defaultPrng().nextCanonicalDouble()) {}

SpanTelemetryContextImpl::SpanTelemetryContextImpl(OtelContext ctx, PseudoRandom* prng)
    : _ctx(std::move(ctx)), _samplingRoll((prng ? *prng : defaultPrng()).nextCanonicalDouble()) {}

SpanTelemetryContextImpl::SpanTelemetryContextImpl(OtelContext ctx, double samplingRoll)
    : _ctx(std::move(ctx)), _samplingRoll(samplingRoll) {}

std::shared_ptr<TelemetryContext> SpanTelemetryContextImpl::clone() const {
    // Clones inherit the parent's roll so all telemetry contexts within one operation share the
    // same sampling decision.
    return std::shared_ptr<TelemetryContext>(
        new SpanTelemetryContextImpl(_getContext(), _samplingRoll));
}

void SpanTelemetryContextImpl::propagate(TextMapPropagator& propagator,
                                         TextMapCarrier& carrier) const {
    propagator.Inject(carrier, _getContext());
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo
