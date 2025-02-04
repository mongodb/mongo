/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <smithy/Smithy_EXPORTS.h>
#include <memory>
#include <utility>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * Status of the span.
             */
            enum class TraceSpanStatus {
                UNSET,
                OK,
                FAULT,
            };

            /**
             * The basic unit of a "Trace". Represents a time period during which events
             * occur. Child spans and events can take place within trace. It is a hierarchy
             * and ledger of timing and events during a operation.
             */
            class SMITHY_API TraceSpan {
            public:
                /**
                 * Create a Span
                 * @param name The name of the span.
                 */
                TraceSpan(Aws::String name) : m_name(std::move(name)) {}

                virtual ~TraceSpan() = default;

                /**
                 * Emit a event associated with the span.
                 * @param name The name of the event.
                 * @param attributes the attributes or dimensions associate with this measurement.
                 */
                virtual void emitEvent(Aws::String name, const Aws::Map<Aws::String, Aws::String> &attributes) = 0;

                /**
                 * Set a Attribute to span.
                 * @param key The key of the dimension/attribute.
                 * @param value The value of the dimension/attribute.
                 */
                virtual void setAttribute(Aws::String key, Aws::String value) = 0;

                /**
                 * Set the statue of the span.
                 * @param status The status to be assigned.
                 */
                virtual void setStatus(TraceSpanStatus status) = 0;

                /**
                 * End the span and mark as finished.
                 */
                virtual void end() = 0;

            private:
                Aws::String m_name;
            };
        }
    }
}