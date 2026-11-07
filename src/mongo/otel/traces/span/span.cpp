// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/otel/traces/span/span.h"
#ifdef MONGO_CONFIG_OTEL

#include "mongo/db/operation_context.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/span/span_telemetry_context_impl.h"
#include "mongo/otel/traces/trace_sampling_parameters.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/otel/traces/tracing_feature_flags_gen.h"
#include "mongo/otel/traces/tracing_utils.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string_view>

#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace otel {
namespace traces {

using OtelStringView = std::string_view;

static constexpr OtelStringView errorCodeKey = "errorCode";
static constexpr OtelStringView errorCodeStringKey = "errorCodeString";

using ScopedSpan = std::shared_ptr<opentelemetry::trace::Span>;

class Span::SpanImpl {
public:
    SpanImpl(ScopedSpan span, ScopedSpan parent, std::shared_ptr<SpanTelemetryContextImpl> spanCtx)
        : _span(std::move(span)), _parent(std::move(parent)), _spanCtx(std::move(spanCtx)) {
        _spanCtx->setSpan(_span);
    }

    ~SpanImpl() {
        _spanCtx->setSpan(std::move(_parent));
        const bool ok = !std::uncaught_exceptions() && !_error;
        _span->SetStatus(ok ? opentelemetry::trace::StatusCode::kOk
                            : opentelemetry::trace::StatusCode::kError);
        _span->End();
    }

    void setAttribute(std::string_view key, int value) {
        _span->SetAttribute(std::string{key}, value);
    }

    void setAttribute(std::string_view key, std::string_view value) {
        _span->SetAttribute(std::string{key}, std::string{value});
    }

    void setStatus(const Status& status) {
        _span->SetAttribute(errorCodeKey, status.code());
        _span->SetAttribute(errorCodeStringKey, status.reason());
        _error = !status.isOK();
    }

private:
    ScopedSpan _span;
    ScopedSpan _parent;
    std::shared_ptr<SpanTelemetryContextImpl> _spanCtx;
    bool _error = false;
};

Span::Span() = default;

Span::Span(std::unique_ptr<Span::SpanImpl> impl) : _impl(std::move(impl)) {}

Span::~Span() = default;

Span& Span::operator=(Span&&) noexcept = default;

Span::Span(Span&&) noexcept = default;

void Span::setAttribute(std::string_view key, int value) {
    if (_impl) {
        _impl->setAttribute(key, value);
    }
}

void Span::setAttribute(std::string_view key, std::string_view value) {
    if (_impl) {
        _impl->setAttribute(key, value);
    }
}

void Span::setStatus(const Status& status) {
    if (_impl) {
        _impl->setStatus(status);
    }
}

static opentelemetry::trace::SpanKind toOtelSpanKind(SpanKind kind) {
    switch (kind) {
        case SpanKind::kServer:
            return opentelemetry::trace::SpanKind::kServer;
        case SpanKind::kClient:
            return opentelemetry::trace::SpanKind::kClient;
        case SpanKind::kProducer:
            return opentelemetry::trace::SpanKind::kProducer;
        case SpanKind::kConsumer:
            return opentelemetry::trace::SpanKind::kConsumer;
        case SpanKind::kInternal:
            return opentelemetry::trace::SpanKind::kInternal;
    }
    MONGO_UNREACHABLE;
}

Span Span::_start(std::shared_ptr<TelemetryContext>& telemetryCtx,
                  SpanName name,
                  StartSpanConfig config) {
    TracerProviderService* tracerProviderService = getGlobalTracerProviderService();
    if (!tracerProviderService) {
        return Span{};
    }

    auto* provider = tracerProviderService->getTracerProvider();
    if (!provider) {
        return Span{};
    }

    ScopedSpan parentSpan = nullptr;
    bool hasParent = false;
    std::shared_ptr<SpanTelemetryContextImpl> spanCtx = nullptr;
    if (telemetryCtx != nullptr) {
        spanCtx = std::dynamic_pointer_cast<SpanTelemetryContextImpl>(telemetryCtx);
        if (!spanCtx) {
            LOGV2(10011700,
                  "TelemetryContext is not of type SpanTelemetryContextImpl",
                  "context_type"_attr = telemetryCtx->type());
            return Span{};
        }
        parentSpan = spanCtx->getSpan();
        hasParent = parentSpan->GetContext().IsValid();
    }

    const auto hasRemoteParent = hasParent && parentSpan->GetContext().IsRemote();

    // A remote parent that was sampled upstream bypasses internal sampling to respect that decision
    // (subject to the external rate limit). A remote parent that was not sampled upstream still
    // goes through internal sampling like a root span.
    if (!hasParent || hasRemoteParent) {
        // Drop if either feature flag is disabled or the sampler rejects it.
        // (Ignore FCV check) — no VersionContext is available in this path.
        if (!feature_flags::gFeatureFlagTracing.isEnabledAndIgnoreFCVUnsafe() ||
            !feature_flags::gFeatureFlagOtelTraceSampling.isEnabledAndIgnoreFCVUnsafe()) {
            return Span{};
        }

        if (config.preventSampling) {
            return Span{};
        }

        auto& sampler = TracingSampler::get();

        // Only spend an external token on remote parents that were sampled upstream. If external
        // tracing is disabled or the external rate limit rejects it, internal sampling may still
        // choose to trace it.
        // The external token is consumed last, so that it is only spent when every other condition
        // for accepting the trace already holds.
        const auto bypassInternalSampling = config.isIngress && isExternalTracingEnabled() &&
            hasRemoteParent && parentSpan->GetContext().IsSampled() &&
            sampler.shouldAcceptExternalTrace();

        if (!bypassInternalSampling) {
            // We need a telemetryCtx for sampling, but it is slightly expensive to create, so do
            // so only if needed.
            if (!telemetryCtx) {
                spanCtx = std::make_shared<SpanTelemetryContextImpl>();
                telemetryCtx = spanCtx;
                parentSpan = spanCtx->getSpan();
            }

            if (!sampler.shouldSample(name.getName(), spanCtx->getSamplingValue())) {
                return Span{};
            }
        }
    }

    // GetTracer() takes a process-wide mutex, so it is only called for spans that will be recorded,
    // after sampling has decided to keep them.
    // TODO: Cache the tracer alongside the provider to avoid the lookup entirely.
    auto tracer = provider->GetTracer("mongodb");
    if (!tracer) {
        return Span{};
    }

    opentelemetry::trace::StartSpanOptions opts;
    opts.parent = parentSpan->GetContext();
    opts.kind = toOtelSpanKind(config.opts.kind);

    return Span(
        std::make_unique<Span::SpanImpl>(tracer->StartSpan(std::string{name.getName()}, opts),
                                         std::move(parentSpan),
                                         std::move(spanCtx)));
}

Span Span::start(std::shared_ptr<TelemetryContext>& telemetryCtx,
                 SpanName name,
                 SpanOptions options) {
    return _start(telemetryCtx, name, {.opts = std::move(options)});
}

Span Span::_start(OperationContext* opCtx, SpanName name, StartSpanConfig config) {
    if (opCtx == nullptr) {
        return Span{};
    }

    auto& telemetryCtxHolder = TelemetryContextHolder::getDecoration(opCtx);
    auto telemetryCtx = telemetryCtxHolder.getTelemetryContext();

    bool hadTelemetryCtx = telemetryCtx != nullptr;

    Span span = _start(telemetryCtx, name, config);

    // Start created a new TelemetryContext, so we need to store it for future use.
    if (!hadTelemetryCtx && telemetryCtx != nullptr) {
        telemetryCtxHolder.setTelemetryContext(telemetryCtx);
    }
    return span;
}

Span Span::start(OperationContext* opCtx, SpanName name, SpanOptions options) {
    return _start(opCtx, name, {.opts = std::move(options)});
}

Span Span::startIngressSpan(std::shared_ptr<TelemetryContext>& telemetryCtx,
                            SpanName name,
                            SpanOptions options) {
    return _start(telemetryCtx, name, {.opts = std::move(options), .isIngress = true});
}

Span Span::startEgressSpan(std::shared_ptr<TelemetryContext>& telemetryCtx,
                           SpanName name,
                           SpanOptions options) {
    return _start(telemetryCtx, name, {.opts = std::move(options), .preventSampling = true});
}

Span Span::startEgressSpan(OperationContext* opCtx, SpanName name, SpanOptions options) {
    return _start(opCtx, name, {.opts = std::move(options), .preventSampling = true});
}

std::shared_ptr<TelemetryContext> Span::createTelemetryContext() {
    return std::make_shared<SpanTelemetryContextImpl>();
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo
#endif
