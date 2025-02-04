/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <cstdlib>

namespace Aws
{
    namespace Utils
    {
        namespace Memory
        {
            /**
             * Central interface for memory management customizations. To create a custom memory manager, implement this interface and then
             * call InitializeAWSMemorySystem().
             */
            class AWS_CORE_API MemorySystemInterface
            {
            public:
                virtual ~MemorySystemInterface() = default;

                /**
                 * This is for initializing your memory manager in a static context. This can be empty if you don't need to do that.
                 */
                virtual void Begin() = 0;
                /**
                * This is for cleaning up your memory manager in a static context. This can be empty if you don't need to do that.
                */
                virtual void End() = 0;

                /**
                 * Allocate your memory inside this method. blocksize and alignment are exactly the same as the std::allocator interfaces.
                 * The allocationTag parameter is for memory tracking; you don't have to handle it.
                 */
                virtual void* AllocateMemory(std::size_t blockSize, std::size_t alignment, const char *allocationTag = nullptr) = 0;

                /**
                 * Free the memory pointed to by memoryPtr.
                 */
                virtual void FreeMemory(void* memoryPtr) = 0;
            };

        } // namespace Memory
    } // namespace Utils
} // namespace Aws
