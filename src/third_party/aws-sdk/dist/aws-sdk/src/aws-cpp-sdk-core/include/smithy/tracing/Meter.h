/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/tracing/Gauge.h>
#include <smithy/tracing/MonotonicCounter.h>
#include <smithy/tracing/UpDownCounter.h>
#include <smithy/tracing/Histogram.h>
#include <smithy/Smithy_EXPORTS.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * Entry point to creating instruments. An Instrument is responsible for taking measurements.
             * The returned entities will be responsible for taking measurements directly in code.
             */
            class SMITHY_API Meter {
            public:
                virtual ~Meter() = default;

                /**
                 * Create a Gauge instrument that will measure a provided callback function.
                 * @param name The metric name being recorded.
                 * @param callback The callback function that will be recording the measurement
                 *                 in a async runtime.
                 * @param units The units of the measurement being recorded.
                 * @param description The description of the measurement.
                 * @return A handle to the gauge that has been created that has access to the
                 *         callback being used to record the metric.
                 */
                virtual Aws::UniquePtr<GaugeHandle> CreateGauge(Aws::String name,
                    std::function<void(Aws::UniquePtr<AsyncMeasurement>)> callback,
                    Aws::String units,
                    Aws::String description) const = 0;

                /**
                 * Create a UpDownCounter that will measure a metric that can increase/decrease
                 * in count.
                 *
                 * @param name The metric name being recorded.
                 * @param units units The units of the measurement being recorded.
                 * @param description The description of the measurement.
                 * @return A UpDownCounter that can record the value of a count.
                 */
                virtual Aws::UniquePtr<UpDownCounter> CreateUpDownCounter(Aws::String name,
                    Aws::String units,
                    Aws::String description) const = 0;

                /**
                 * Create a Counter that will measure a metric that can only increase
                 * in count.
                 *
                 * @param name The metric name being recorded.
                 * @param units units The units of the measurement being recorded.
                 * @param description The description of the measurement.
                 * @return A Counter that can record the value of a count.
                 */
                virtual Aws::UniquePtr<MonotonicCounter> CreateCounter(Aws::String name,
                    Aws::String units,
                    Aws::String description) const = 0;

                /**
                 * Create a Histogram that will measure a metric that can be translated
                 * into a statistical measurement.
                 *
                 * @param name The metric name being recorded.
                 * @param units units The units of the measurement being recorded.
                 * @param description The description of the measurement.
                 * @return A Histogram that can measure a statistical value.
                 */
                virtual Aws::UniquePtr<Histogram> CreateHistogram(Aws::String name,
                    Aws::String units,
                    Aws::String description) const = 0;
            };
        }
    }
}