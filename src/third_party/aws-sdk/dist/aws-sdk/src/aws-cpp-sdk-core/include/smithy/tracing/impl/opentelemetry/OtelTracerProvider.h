/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>
#include <smithy/tracing/TracerProvider.h>
#include <smithy/tracing/Tracer.h>
#include <smithy/tracing/TraceSpan.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A Open Telemetry Implementation of TracerProvider.
             */
            class SMITHY_API OtelTracerProvider final : public TracerProvider {
            public:
                std::shared_ptr<Tracer> GetTracer(Aws::String scope,
                    const Aws::Map<Aws::String,
                        Aws::String> &attributes) override;
            };
        }
    }
}