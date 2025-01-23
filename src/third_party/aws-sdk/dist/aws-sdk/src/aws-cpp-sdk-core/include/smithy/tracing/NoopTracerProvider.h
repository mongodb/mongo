/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>
#include <smithy/tracing/TracerProvider.h>


namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A no-op implementation of TraceSpan that is pass through.
             */
            class NoopTracerSpan : public TraceSpan {
            public:
                NoopTracerSpan(const Aws::String &name) : TraceSpan(name) {}

                void emitEvent(Aws::String name, const Aws::Map<Aws::String, Aws::String> &attributes) override {
                    AWS_UNREFERENCED_PARAM(name);
                    AWS_UNREFERENCED_PARAM(attributes);
                }

                void setAttribute(Aws::String key, Aws::String value) override {
                    AWS_UNREFERENCED_PARAM(key);
                    AWS_UNREFERENCED_PARAM(value);
                }

                void setStatus(TraceSpanStatus status) override {
                    AWS_UNREFERENCED_PARAM(status);
                }

                void end() override {}
            };

            /**
             * A no-op implementation of Tracer that is pass through.
             */
            class NoopTracer : public Tracer {
            public:
                std::shared_ptr<TraceSpan> CreateSpan(Aws::String name,
                    const Aws::Map<Aws::String, Aws::String> &attributes,
                    SpanKind spanKind) override {
                    AWS_UNREFERENCED_PARAM(attributes);
                    AWS_UNREFERENCED_PARAM(spanKind);
                    return Aws::MakeShared<NoopTracerSpan>("NO_OP", name);
                }
            };

            /**
             * A no-op implementation of NoopTracerProvider that is pass through.
             */
            class NoopTracerProvider : public TracerProvider {
            public:
                explicit NoopTracerProvider(std::shared_ptr<NoopTracer> tracer) : m_tracer(std::move(tracer)) {}

                std::shared_ptr<Tracer> GetTracer(Aws::String scope,
                    const Aws::Map<Aws::String, Aws::String> &attributes) override {
                    AWS_UNREFERENCED_PARAM(scope);
                    AWS_UNREFERENCED_PARAM(attributes);
                    return m_tracer;
                }

            private:
                std::shared_ptr<NoopTracer> m_tracer;
            };
        }
    }
}