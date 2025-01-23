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
             * Measures a value that only ever increases.
             */
            class SMITHY_API MonotonicCounter {
            public:
                virtual ~MonotonicCounter() = default;

                /**
                 * Adds a value to counter.
                 * @param value the count to be added to the counter.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 */
                virtual void add(long value, Aws::Map<Aws::String, Aws::String> attributes) = 0;
            };
        }
    }
}