/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>
#include <smithy/tracing/Tracer.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/tracer.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/scope.h>
#include <memory>
#include <utility>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A Open Telemetry Implementation of TraceSpan.
             */
            class OtelSpanAdapter final : public TraceSpan {
            public:
                explicit OtelSpanAdapter(Aws::String name,
                    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span,
                    opentelemetry::trace::Scope scope) :
                    TraceSpan(std::move(name)), otelSpan(std::move(span)), otelScope(std::move(scope)) {}

                ~OtelSpanAdapter() override;

                void emitEvent(Aws::String name, const Aws::Map<Aws::String, Aws::String> &attributes) override;

                void setAttribute(Aws::String key, Aws::String value) override;

                void setStatus(TraceSpanStatus status) override;

                void end() override;

            private:
                opentelemetry::trace::StatusCode convertStatusCode(TraceSpanStatus status);

                opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> otelSpan;
                opentelemetry::trace::Scope otelScope;
            };

            /**
             * A Open Telemetry Implementation of Tracer.
             */
            class OtelTracerAdapter final : public Tracer {
            public:
                explicit OtelTracerAdapter(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer) :
                    otelTracer(std::move(tracer)) {}

                std::shared_ptr<TraceSpan> CreateSpan(Aws::String name,
                    const Aws::Map<Aws::String, Aws::String> &attributes,
                    SpanKind spanKind) override;

            private:
                opentelemetry::trace::SpanKind convertSpanKind(SpanKind status);

                opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> otelTracer;
            };

        }
    }
}