/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/tracing/Tracer.h>
#include <smithy/Smithy_EXPORTS.h>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * Entry point for creating Tracer instances.
             */
            class SMITHY_API TracerProvider {
            public:
                virtual ~TracerProvider() = default;

                /**
                 * Returns a reference to a Tracer instance.
                 * @param scope The scope of the Tracer that is being used.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 * @return A reference to a Tracer instance.
                 */
                virtual std::shared_ptr<Tracer> GetTracer(Aws::String scope, const Aws::Map<Aws::String, Aws::String> &attributes) = 0;
            };
        }
    }
}