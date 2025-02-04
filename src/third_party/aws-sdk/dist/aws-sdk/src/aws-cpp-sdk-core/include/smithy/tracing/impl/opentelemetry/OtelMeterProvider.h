/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>
#include <smithy/tracing/MeterProvider.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A Open Telemetry Implementation of MeterProvider.
             */
            class SMITHY_API OtelMeterProvider final : public MeterProvider {
            public:
                std::shared_ptr<Meter> GetMeter(Aws::String scope,
                    Aws::Map<Aws::String,
                    Aws::String> attributes) override;
            };
        }
    }
}