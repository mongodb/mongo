/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>
#include <smithy/tracing/TracerProvider.h>
#include <smithy/tracing/MeterProvider.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <utility>
#include <mutex>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A Utility holder class that manages the creation and
             * management of telemetry related operations.
             */
            class SMITHY_API TelemetryProvider {
            public:
                /**
                 * Creates a Telemetry provider with given providers and a init
                 * and shutdown function that is run during its ctor/dtor.
                 * @param tracerProvider The TracerProvider to be used in the SDK.
                 * @param meterProvider The MeterProvider to be used in the SDK.
                 * @param init The initialization function that will be run at creation.
                 * @param shutdown The shutdown function that will be run at destruction.
                 */
                TelemetryProvider(Aws::UniquePtr<TracerProvider> tracerProvider,
                    Aws::UniquePtr<MeterProvider> meterProvider,
                    std::function<void()> init,
                    std::function<void()> shutdown) :
                    m_tracerProvider(std::move(tracerProvider)),
                    m_meterProvider(std::move(meterProvider)),
                    m_init(std::move(init)),
                    m_shutdown(std::move(shutdown))
                {
                    RunInit();
                }

                virtual ~TelemetryProvider() {
                    RunShutDown();
                }

                /**
                 * Returns a reference to a Tracer used to create spans.
                 *
                 * @param scope The scope of the Tracer that is being used.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 * @return A reference to a Tracer instance.
                 */
                std::shared_ptr<Tracer>
                getTracer(Aws::String scope, const Aws::Map<Aws::String, Aws::String> &attributes) {
                    return m_tracerProvider->GetTracer(std::move(scope), attributes);
                }

                /**
                 * Returns a reference to a Meter used to create metrics.
                 *
                 * @param scope The scope of the Meter that is being used.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 * @return A reference to a Meter instance.
                 */
                std::shared_ptr<Meter>
                getMeter(Aws::String scope, const Aws::Map<Aws::String, Aws::String> &attributes) {
                    return m_meterProvider->GetMeter(std::move(scope), attributes);
                }

                /**
                 * Runs initialization of the Tracer Provider and the MeterProvider. Will only
                 * be run once during initialization.
                 */
                void RunInit() {
                    std::call_once(m_initFlag, m_init);
                }

                /**
                 * Runs shutdown of the Tracer Provider and the MeterProvider. Will only
                 * be run once during destruction.
                 */
                void RunShutDown() {
                    std::call_once(m_shutdownFlag, m_shutdown);
                }

            private:
                std::once_flag m_initFlag;
                std::once_flag m_shutdownFlag;
                const Aws::UniquePtr<TracerProvider> m_tracerProvider;
                const Aws::UniquePtr<MeterProvider> m_meterProvider;
                const std::function<void()> m_init;
                const std::function<void()> m_shutdown;
            };
        }
    }
}