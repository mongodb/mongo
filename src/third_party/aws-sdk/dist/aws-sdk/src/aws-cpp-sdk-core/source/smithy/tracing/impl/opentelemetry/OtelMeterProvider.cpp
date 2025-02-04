/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <smithy/tracing/impl/opentelemetry/OtelMeterProvider.h>
#include <smithy/tracing/impl/opentelemetry/MeterAdapters.h>
#include <opentelemetry/metrics/provider.h>

using namespace smithy::components::tracing;

static const char * ALLOC_TAG = "OTEL_METER_PROVIDER";

std::shared_ptr<Meter> OtelMeterProvider::GetMeter(Aws::String scope,
    Aws::Map<Aws::String, Aws::String> attributes) {
    AWS_UNREFERENCED_PARAM(attributes);
    auto otelMeterProvider = opentelemetry::metrics::Provider::GetMeterProvider();
    auto meter = otelMeterProvider->GetMeter(scope);
    return Aws::MakeShared<OtelMeterAdapter>(ALLOC_TAG, meter);
}