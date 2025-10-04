#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Allocator.h>

#include <memory>
#include <type_traits>

namespace Aws
{
    namespace Crt
    {
        /**
         * Stateful allocator variant that uses an underlying CRT allocator
         * @tparam T type that allocator can allocate
         */
        template <typename T> class StlAllocator : public std::allocator<T>
        {
          public:
            using Base = std::allocator<T>;

            StlAllocator() noexcept : Base() { m_allocator = ApiAllocator(); }

            StlAllocator(Allocator *allocator) noexcept : Base() { m_allocator = allocator; }

            StlAllocator(const StlAllocator<T> &a) noexcept : Base(a) { m_allocator = a.m_allocator; }

            template <class U> StlAllocator(const StlAllocator<U> &a) noexcept : Base(a)
            {
                m_allocator = a.m_allocator;
            }

            ~StlAllocator() {}

            using size_type = std::size_t;

            template <typename U> struct rebind
            {
                typedef StlAllocator<U> other;
            };

            using RawPointer = typename std::allocator_traits<std::allocator<T>>::pointer;

            RawPointer allocate(size_type n, const void *hint = nullptr)
            {
                (void)hint;
                AWS_ASSERT(m_allocator);
                return static_cast<RawPointer>(aws_mem_acquire(m_allocator, n * sizeof(T)));
            }

#if _LIBCPP_STD_VER > 20
            std::allocation_result<T *> allocate_at_least(size_type n) { return {allocate(n), n}; }
#endif

            void deallocate(RawPointer p, size_type)
            {
                AWS_ASSERT(m_allocator);
                aws_mem_release(m_allocator, p);
            }

            Allocator *m_allocator;
        };
    } // namespace Crt
} // namespace Aws
