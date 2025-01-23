/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Allocator.h>

namespace Aws
{
    namespace Crt
    {

        Allocator *DefaultAllocatorImplementation() noexcept
        {
            return aws_default_allocator();
        }

        Allocator *DefaultAllocator() noexcept
        {
            return DefaultAllocatorImplementation();
        }

        Allocator *g_allocator = Aws::Crt::DefaultAllocatorImplementation();

        Allocator *ApiAllocator() noexcept
        {
            return g_allocator;
        }

    } // namespace Crt
} // namespace Aws
