/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/SDKConfig.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/MemorySystemInterface.h>
#include <aws/crt/StlAllocator.h>

#include <memory>
#include <cstdlib>

namespace Aws
{
#ifdef USE_AWS_MEMORY_MANAGEMENT
    /**
     * Std allocator interface that is used for all STL types in CRT library
     * in the event that Custom Memory Management is being used.
     *
     * Both SDK and CRT STL allocators are going to call the same SDK memory allocation methods.
     * However, we should keep them separated
     * to allow SDK to use the allocator when CRT is not initialized or already terminated.
     */
    template< typename T > using CrtAllocator = Aws::Crt::StlAllocator<T>;

    /**
     * Std allocator interface that is used for all STL types in the SDK
     * in the event that Custom Memory Management is being used.
     */  
    template <typename T>
    class Allocator : public std::allocator<T>
    {
    public:

        typedef std::allocator<T> Base;

        Allocator() throw() :
            Base()
        {}

        Allocator(const Allocator<T>& a) throw() :
            Base(a)
        {}

        template <class U>
        Allocator(const Allocator<U>& a) throw() :
            Base(a)
        {}

        ~Allocator() throw() {}

        typedef std::size_t size_type;

        template<typename U>
        struct rebind
        {
            typedef Allocator<U> other;
        };

        using RawPointer = typename std::allocator_traits<std::allocator<T>>::pointer;

        RawPointer allocate(size_type n, const void *hint = nullptr)
        {
            AWS_UNREFERENCED_PARAM(hint);

            return reinterpret_cast<RawPointer>(Malloc("AWSSTL", n * sizeof(T)));
        }

        void deallocate(RawPointer p, size_type n)
        {
            AWS_UNREFERENCED_PARAM(n);

            Free(p);
        }

    };

#ifdef __ANDROID__
#if _GLIBCXX_FULLY_DYNAMIC_STRING == 0
    template< typename T >
    bool operator ==(const Allocator< T >& lhs, const Allocator< T >& rhs)
    {
        AWS_UNREFERENCED_PARAM(lhs);
        AWS_UNREFERENCED_PARAM(rhs);

        return false;
    }
#endif // _GLIBCXX_FULLY_DYNAMIC_STRING == 0
#endif // __ANDROID__

#else

    template< typename T > using Allocator = std::allocator<T>;

#endif // USE_AWS_MEMORY_MANAGEMENT
    /**
     * Creates a shared_ptr using AWS Allocator hooks.
     * allocationTag is for memory tracking purposes.
     */
    template<typename T, typename ...ArgTypes>
    std::shared_ptr<T> MakeShared(const char* allocationTag, ArgTypes&&... args)
    {
#ifdef USE_AWS_MEMORY_MANAGEMENT
        Aws::Utils::Memory::MemorySystemInterface* memorySystem = Aws::Utils::Memory::GetMemorySystem();
        // Was InitAPI forgotten or ShutdownAPI already called or Aws:: class used as static?
        // TODO: enforce to non-conditional assert
        AWS_ASSERT(memorySystem && "Memory system is not initialized.");
        AWS_UNREFERENCED_PARAM(memorySystem);
#endif
        AWS_UNREFERENCED_PARAM(allocationTag);

        return std::allocate_shared<T, Aws::Allocator<T>>(Aws::Allocator<T>(), std::forward<ArgTypes>(args)...);
    }


} // namespace Aws
