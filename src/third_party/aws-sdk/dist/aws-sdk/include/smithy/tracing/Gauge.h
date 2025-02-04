/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <smithy/Smithy_EXPORTS.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A container that starts a async measurement
             * that has virtual function to stop it.
             */
            class SMITHY_API GaugeHandle {
            public:
                virtual ~GaugeHandle() = default;

                /**
                 * Stop the measurement of the gauge.
                 */
                virtual void Stop() = 0;
            };

            /**
             * Measures the current instantaneous value. Is used in
             * the implementation of a gauge handle to take an actual
             * measurement.
             */
            class SMITHY_API AsyncMeasurement {
            public:
                virtual ~AsyncMeasurement() = default;

                /**
                 * A Functional interface of recording a value.
                 * @param value  the value of the measurement.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 */
                virtual void Record(double value, const Aws::Map<Aws::String, Aws::String> &attributes) = 0;
            };
        }
    }
}