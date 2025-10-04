/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <stdint.h>
#include <chrono>

namespace Aws
{
    namespace Utils
    {
        namespace RateLimits
        {
            /**
             * Interface for bandwidth limiters. You likely don't want to implement your own as DefaultRateLimiter is probably what you need.
             * If you need to implement one, then this is the interface to implement.
             */
            class RateLimiterInterface
            {
            public:
                using DelayType = std::chrono::milliseconds;

                virtual ~RateLimiterInterface() {}
                /**
                * Calculates time in milliseconds that should be delayed before letting anymore data through.
                */
                virtual DelayType ApplyCost(int64_t cost) = 0;
                /**
                * Same as ApplyCost() but then goes ahead and sleeps the current thread.
                */
                virtual void ApplyAndPayForCost(int64_t cost) = 0;
                /**
                * Update the bandwidth rate to allow.
                */
                virtual void SetRate(int64_t rate, bool resetAccumulator = false) = 0;
            };

        } // namespace RateLimits
    } // namespace Utils
} // namespace Aws