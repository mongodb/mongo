/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>

#include <utility>
#include <smithy/tracing/TelemetryProvider.h>
#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/metrics/push_metric_exporter.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A Open Telemetry Implementation of TelemetryProvider.
             */
            class SMITHY_API OtelTelemetryProvider final : public TelemetryProvider {
            public:
                static Aws::UniquePtr<TelemetryProvider> CreateOtelProvider(
                    std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> spanExporter,
                    std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> pushMetricExporter);
            };
        }
    }
}