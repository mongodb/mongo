/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/tracing/TraceSpan.h>
#include <smithy/Smithy_EXPORTS.h>
#include <memory>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * The kind of span being created.
             */
            enum class SpanKind {
                INTERNAL,
                CLIENT,
                SERVER,
            };

            /**
             * Entry point for creating a Span. Any new spans will
             * be created from this.
             */
            class SMITHY_API Tracer {
            public:
                virtual ~Tracer() = default;

                /**
                 * Creates a span.
                 *
                 * @param name Name of the span.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 * @param spanKind The kind of the span.
                 * @return A instance of a span.
                 */
                virtual std::shared_ptr<TraceSpan> CreateSpan(Aws::String name,
                    const Aws::Map<Aws::String, Aws::String> &attributes,
                    SpanKind spanKind) = 0;
            };
        }
    }
}