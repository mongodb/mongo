/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>
#include <smithy/tracing/Meter.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/observer_result.h>
#include <opentelemetry/metrics/async_instruments.h>
#include <utility>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A Open Telemetry Implementation of Meter.
             */
            class OtelMeterAdapter final : public Meter {
            public:
                explicit OtelMeterAdapter(opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> meter) :
                    otelMeter(std::move(meter)) {}

                Aws::UniquePtr<GaugeHandle> CreateGauge(Aws::String name,
                    std::function<void(Aws::UniquePtr<AsyncMeasurement>)> callback,
                    Aws::String units,
                    Aws::String description) const override;

                Aws::UniquePtr<UpDownCounter> CreateUpDownCounter(Aws::String name,
                    Aws::String units,
                    Aws::String description) const override;

                Aws::UniquePtr<MonotonicCounter> CreateCounter(Aws::String name,
                    Aws::String units,
                    Aws::String description) const override;

                Aws::UniquePtr<Histogram> CreateHistogram(Aws::String name,
                    Aws::String units,
                    Aws::String description) const override;

            private:
                opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> otelMeter;
            };

            /**
             * A Open Telemetry Implementation of MonotonicCounter.
             */
            class OtelCounterAdapter final : public MonotonicCounter {
            public:
                explicit OtelCounterAdapter(
                    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>> counter);

                void add(long value, Aws::Map<Aws::String, Aws::String> attributes) override;

            private:
                opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>> otelCounter;
            };

            /**
             * A Open Telemetry Implementation of UpDownCounter.
             */
            class OtelUpDownCounterAdapter final : public UpDownCounter {
            public:
                explicit OtelUpDownCounterAdapter(
                    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<int64_t>> counter);

                void add(long value, Aws::Map<Aws::String, Aws::String> attributes) override;

            private:
                opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<int64_t>> otelUpDownCounter;
            };

            /**
             * A Open Telemetry Implementation of Histogram.
             */
            class OtelHistogramAdapter final : public Histogram {
            public:
                explicit OtelHistogramAdapter(
                    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> otelHistogram);

                void record(double value,
                    Aws::Map<Aws::String, Aws::String> attributes) override;

            private:
                opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> otelHistogram;
            };

            /**
             * A struct type for the C function pointer interface to pass state.
             */
            struct GaugeHandleState {
                std::function<void(Aws::UniquePtr<AsyncMeasurement>)> callback;
            };

            /**
             * A Open Telemetry Implementation of GaugeHandle.
             */
            class OtelGaugeAdapter final : public GaugeHandle {
            public:
                explicit OtelGaugeAdapter(
                    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> otelGauge,
                        opentelemetry::metrics::ObservableCallbackPtr callback);

                void Stop() override;

            private:
                opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> otelGauge;
                opentelemetry::metrics::ObservableCallbackPtr otelCallback;
            };

            /**
             * A Open Telemetry Implementation of AsyncMeasurement.
             */
            class OtelObserverAdapter final : public AsyncMeasurement {
            public:
                explicit OtelObserverAdapter(const opentelemetry::metrics::ObserverResult &otelResult);

                void Record(double value, const Aws::Map<Aws::String, Aws::String> &attributes) override;

            private:
                const opentelemetry::metrics::ObserverResult &otelResult;
            };
        }
    }
}