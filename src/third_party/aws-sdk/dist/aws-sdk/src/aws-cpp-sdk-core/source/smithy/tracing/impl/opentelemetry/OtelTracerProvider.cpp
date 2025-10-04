/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <smithy/tracing/impl/opentelemetry/OtelTracerProvider.h>
#include <smithy/tracing/impl/opentelemetry/TraceAdapters.h>
#include <opentelemetry/trace/provider.h>

using namespace smithy::components::tracing;

static const char * ALLOC_TAG = "OTEL_TRACER_PROVIDER";

std::shared_ptr<Tracer> OtelTracerProvider::GetTracer(Aws::String scope,
    const Aws::Map<Aws::String, Aws::String> &attributes) {
    AWS_UNREFERENCED_PARAM(attributes);
    auto otelTracerProvider = opentelemetry::trace::Provider::GetTracerProvider();
    auto otelTracer = otelTracerProvider->GetTracer(scope);
    return Aws::MakeShared<OtelTracerAdapter>(ALLOC_TAG, otelTracer);
}
