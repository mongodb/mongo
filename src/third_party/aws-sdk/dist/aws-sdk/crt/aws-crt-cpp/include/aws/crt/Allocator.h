#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/crt/Exports.h>

namespace Aws
{
    namespace Crt
    {
        using Allocator = aws_allocator;

        /**
         * Each object from this library can use an explicit allocator.
         * If you construct an object without specifying an allocator,
         * then THIS allocator is used instead.
         *
         * You can customize this allocator when initializing
         * \ref ApiHandle::ApiHandle(Allocator*) "ApiHandle".
         */
        AWS_CRT_CPP_API Allocator *ApiAllocator() noexcept;

        /**
         * Returns the default implementation of an Allocator.
         *
         * If you initialize \ref ApiHandle::ApiHandle(Allocator*) "ApiHandle"
         * without specifying a custom allocator, then this implementation is used.
         */
        AWS_CRT_CPP_API Allocator *DefaultAllocatorImplementation() noexcept;

        /**
         * @deprecated Use DefaultAllocatorImplementation() instead.
         * DefaultAllocator() is too easily confused with ApiAllocator().
         */
        AWS_CRT_CPP_API Allocator *DefaultAllocator() noexcept;

        /**
         * @deprecated Use ApiAllocator() instead, to avoid issues with delay-loaded DLLs.
         * https://github.com/aws/aws-sdk-cpp/issues/1960
         */
        extern AWS_CRT_CPP_API Allocator *g_allocator;

    } // namespace Crt
} // namespace Aws
