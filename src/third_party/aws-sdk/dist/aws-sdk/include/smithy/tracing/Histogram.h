/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <smithy/Smithy_EXPORTS.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * Measures a value where the statistics are likely meaningful.
             */
            class SMITHY_API Histogram {
            public:
                virtual ~Histogram() = default;

                /**
                 * Records a value to the histogram.
                 *
                 * @param value the value that be recorded in a statistical distribution.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 */
                virtual void record(double value, Aws::Map<Aws::String, Aws::String> attributes) = 0;
            };
        }
    }
}