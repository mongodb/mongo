/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <smithy/Smithy_EXPORTS.h>
#include <smithy/tracing/Meter.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * Entry point for metrics emission API. Will return a meter which in turn
             * can provide specific metric taking instruments.
             */
            class SMITHY_API MeterProvider {
            public:
                virtual ~MeterProvider() = default;

                /**
                 * Provide a meter that will in turn provide instruments for metrics.
                 * @param scope The scope that meter is used for.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 * @return A Meter.
                 */
                virtual std::shared_ptr<Meter> GetMeter(Aws::String scope,
                    Aws::Map<Aws::String, Aws::String> attributes) = 0;
            };
        }
    }
}