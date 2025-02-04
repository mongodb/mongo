/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <smithy/tracing/impl/opentelemetry/TraceAdapters.h>

using namespace smithy::components::tracing;

static const char *ALLOC_TAG = "OTEL_ADAPTER";

std::shared_ptr<TraceSpan> OtelTracerAdapter::CreateSpan(Aws::String name,
    const Aws::Map<Aws::String, Aws::String> &attributes,
    SpanKind spanKind) {
    opentelemetry::trace::StartSpanOptions spanOptions;
    spanOptions.kind = convertSpanKind(spanKind);
    spanOptions.parent = opentelemetry::context::RuntimeContext::GetCurrent();
    auto otelSpan = otelTracer->StartSpan(name, attributes, {}, spanOptions);
    auto scope = otelTracer->WithActiveSpan(otelSpan);
    return Aws::MakeShared<OtelSpanAdapter>(ALLOC_TAG, name, otelSpan, std::move(scope));
}

opentelemetry::trace::SpanKind OtelTracerAdapter::convertSpanKind(SpanKind status) {
    if (status == SpanKind::SERVER) {
        return opentelemetry::trace::SpanKind::kServer;
    } else if (status == SpanKind::INTERNAL) {
        return opentelemetry::trace::SpanKind::kInternal;
    } else if (status == SpanKind::CLIENT) {
        return opentelemetry::trace::SpanKind::kClient;
    }
    return opentelemetry::trace::SpanKind::kClient;
}

OtelSpanAdapter::~OtelSpanAdapter() {
    end();
}

void OtelSpanAdapter::emitEvent(Aws::String name, const Aws::Map<Aws::String, Aws::String> &attributes) {
    otelSpan->AddEvent(name, attributes);
}

void OtelSpanAdapter::setStatus(smithy::components::tracing::TraceSpanStatus status) {
    otelSpan->SetStatus(convertStatusCode(status));
}

void OtelSpanAdapter::setAttribute(Aws::String key, Aws::String value) {
    otelSpan->SetAttribute(key, value);
}

void OtelSpanAdapter::end() {
    otelSpan->End();
}

opentelemetry::trace::StatusCode OtelSpanAdapter::convertStatusCode(TraceSpanStatus status) {
    if (status == TraceSpanStatus::OK) {
        return opentelemetry::trace::StatusCode::kOk;
    } else if (status == TraceSpanStatus::FAULT) {
        return opentelemetry::trace::StatusCode::kError;
    } else if (status == TraceSpanStatus::UNSET) {
        return opentelemetry::trace::StatusCode::kUnset;
    }
    return opentelemetry::trace::StatusCode::kOk;
}
