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
             * A no-op implementation of the Gauge handle that
             * is simply pass though
             */
            class NoopGaugeHandle : public GaugeHandle {
            public:
                void Stop() override {}
            };

            /**
             * A no-op implementation of the UpDownCounter that
             * is simply pass though
             */
            class NoopUpDownCounter : public UpDownCounter {
                void add(long value, Aws::Map<Aws::String, Aws::String> attributes) override {
                    AWS_UNREFERENCED_PARAM(value);
                    AWS_UNREFERENCED_PARAM(attributes);
                }
            };

            /**
             * A no-op implementation of the MonotonicCounter that
             * is simply pass though
             */
            class NoopMonotonicCounter : public MonotonicCounter {
                void add(long value, Aws::Map<Aws::String, Aws::String> attributes) override {
                    AWS_UNREFERENCED_PARAM(value);
                    AWS_UNREFERENCED_PARAM(attributes);
                }
            };

            /**
             * A no-op implementation of the Histogram that
             * is simply pass though
             */
            class NoopHistogram : public Histogram {
            public:
                void record(double value, Aws::Map<Aws::String, Aws::String> attributes) override {
                    AWS_UNREFERENCED_PARAM(value);
                    AWS_UNREFERENCED_PARAM(attributes);
                }
            };

            /**
             * A no-op implementation of the Meter that
             * is simply pass though
             */
            class NoopMeter : public Meter {
            public:
                Aws::UniquePtr<GaugeHandle> CreateGauge(Aws::String name,
                    std::function<void(Aws::UniquePtr<AsyncMeasurement>)> callback,
                    Aws::String units,
                    Aws::String description) const override
                {
                    AWS_UNREFERENCED_PARAM(name);
                    AWS_UNREFERENCED_PARAM(callback);
                    AWS_UNREFERENCED_PARAM(units);
                    AWS_UNREFERENCED_PARAM(description);
                    return Aws::MakeUnique<NoopGaugeHandle>("NO_OP");
                }

                Aws::UniquePtr<UpDownCounter> CreateUpDownCounter(Aws::String name,
                    Aws::String units,
                    Aws::String description) const override
                {
                    AWS_UNREFERENCED_PARAM(name);
                    AWS_UNREFERENCED_PARAM(units);
                    AWS_UNREFERENCED_PARAM(description);
                    return Aws::MakeUnique<NoopUpDownCounter>("NO_OP");
                }

                Aws::UniquePtr<MonotonicCounter> CreateCounter(Aws::String name,
                    Aws::String units,
                    Aws::String description) const override
                {
                    AWS_UNREFERENCED_PARAM(name);
                    AWS_UNREFERENCED_PARAM(units);
                    AWS_UNREFERENCED_PARAM(description);
                    return Aws::MakeUnique<NoopMonotonicCounter>("NO_OP");
                }

                Aws::UniquePtr<Histogram> CreateHistogram(Aws::String name,
                    Aws::String units,
                    Aws::String description) const override
                {
                    AWS_UNREFERENCED_PARAM(name);
                    AWS_UNREFERENCED_PARAM(units);
                    AWS_UNREFERENCED_PARAM(description);
                    return Aws::MakeUnique<NoopHistogram>("NO_OP");
                }
            };

            /**
             * A no-op implementation of the MeterProvider that
             * is simply pass though
             */
            class NoopMeterProvider : public MeterProvider {
            public:
                std::shared_ptr<Meter>
                GetMeter(Aws::String scope,Aws::Map<Aws::String,Aws::String> attributes) override
                {
                    AWS_UNREFERENCED_PARAM(scope);
                    AWS_UNREFERENCED_PARAM(attributes);
                    return Aws::MakeShared<NoopMeter>("NO_OP");
                }
            };
        }
    }
}