/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>

#include <utility>
#include <smithy/tracing/NoopTracerProvider.h>
#include <smithy/tracing/NoopMeterProvider.h>
#include <smithy/tracing/TelemetryProvider.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A no-op implementation of TelemetryProvider that
             * is simply pass though container of telemetry functionality
             */
            class SMITHY_API NoopTelemetryProvider {
            public:
                static Aws::UniquePtr<TelemetryProvider> CreateProvider() {
                    return Aws::MakeUnique<TelemetryProvider>("NO_OP",
                        Aws::MakeUnique<NoopTracerProvider>("NO_OP", Aws::MakeUnique<NoopTracer>("NO_OP")),
                        Aws::MakeUnique<NoopMeterProvider>("NO_OP"),
                        []() -> void {},
                        []() -> void {});
                }
            };
        }
    }
}