/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2009 Helge Bahmann
 * Copyright (c) 2012 Tim Blechmann
 * Copyright (c) 2014 Andrey Semashev
 */
/*!
 * \file   atomic/detail/ops_gcc_x86_dcas.hpp
 *
 * This header contains implementation of the double-width CAS primitive for x86.
 */

#ifndef BOOST_ATOMIC_DETAIL_OPS_GCC_X86_DCAS_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_OPS_GCC_X86_DCAS_HPP_INCLUDED_

#include <boost/cstdint.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/storage_type.hpp>
#include <boost/atomic/capabilities.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace atomics {
namespace detail {

#if defined(BOOST_ATOMIC_DETAIL_X86_HAS_CMPXCHG8B)

template< bool Signed >
struct gcc_dcas_x86
{
    typedef typename make_storage_type< 8u, Signed >::type storage_type;

    static BOOST_FORCEINLINE void store(storage_type volatile& storage, storage_type v, memory_order) BOOST_NOEXCEPT
    {
        if ((((uint32_t)&storage) & 0x00000007) == 0)
        {
#if defined(__SSE2__)
            __asm__ __volatile__
            (
#if defined(__AVX__)
                "vmovq %1, %%xmm4\n\t"
                "vmovq %%xmm4, %0\n\t"
#else
                "movq %1, %%xmm4\n\t"
                "movq %%xmm4, %0\n\t"
#endif
                : "=m" (storage)
                : "m" (v)
                : "memory", "xmm4"
            );
#else
            __asm__ __volatile__
            (
                "fildll %1\n\t"
                "fistpll %0\n\t"
                : "=m" (storage)
                : "m" (v)
                : "memory"
            );
#endif
        }
        else
        {
#if defined(__PIC__)
            uint32_t scratch;
            __asm__ __volatile__
            (
                "movl %%ebx, %[scratch]\n\t"
                "movl %[value_lo], %%ebx\n\t"
                "movl 0(%[dest]), %%eax\n\t"
                "movl 4(%[dest]), %%edx\n\t"
                ".align 16\n\t"
                "1: lock; cmpxchg8b 0(%[dest])\n\t"
                "jne 1b\n\t"
                "movl %[scratch], %%ebx"
                : [scratch] "=m,m" (scratch)
                : [value_lo] "a,a" ((uint32_t)v), "c,c" ((uint32_t)(v >> 32)), [dest] "D,S" (&storage)
                : "cc", "edx", "memory"
            );
#else
            __asm__ __volatile__
            (
                "movl 0(%[dest]), %%eax\n\t"
                "movl 4(%[dest]), %%edx\n\t"
                ".align 16\n\t"
                "1: lock; cmpxchg8b 0(%[dest])\n\t"
                "jne 1b\n\t"
                :
                : [value_lo] "b,b" ((uint32_t)v), "c,c" ((uint32_t)(v >> 32)), [dest] "D,S" (&storage)
                : "cc", "eax", "edx", "memory"
            );
#endif
        }
    }

    static BOOST_FORCEINLINE storage_type load(storage_type const volatile& storage, memory_order) BOOST_NOEXCEPT
    {
        storage_type value;

        if ((((uint32_t)&storage) & 0x00000007) == 0)
        {
#if defined(__SSE2__)
            __asm__ __volatile__
            (
#if defined(__AVX__)
                "vmovq %1, %%xmm4\n\t"
                "vmovq %%xmm4, %0\n\t"
#else
                "movq %1, %%xmm4\n\t"
                "movq %%xmm4, %0\n\t"
#endif
                : "=m" (value)
                : "m" (storage)
                : "memory", "xmm4"
            );
#else
            __asm__ __volatile__
            (
                "fildll %1\n\t"
                "fistpll %0\n\t"
                : "=m" (value)
                : "m" (storage)
                : "memory"
            );
#endif
        }
        else
        {
#if defined(__clang__)
            // Clang cannot allocate eax:edx register pairs but it has sync intrinsics
            value = __sync_val_compare_and_swap(&storage, (storage_type)0, (storage_type)0);
#else
            // We don't care for comparison result here; the previous value will be stored into value anyway.
            // Also we don't care for ebx and ecx values, they just have to be equal to eax and edx before cmpxchg8b.
            __asm__ __volatile__
            (
                "movl %%ebx, %%eax\n\t"
                "movl %%ecx, %%edx\n\t"
                "lock; cmpxchg8b %[storage]"
                : "=&A" (value)
                : [storage] "m" (storage)
                : "cc", "memory"
            );
#endif
        }

        return value;
    }

    static BOOST_FORCEINLINE bool compare_exchange_strong(
        storage_type volatile& storage, storage_type& expected, storage_type desired, memory_order, memory_order) BOOST_NOEXCEPT
    {
#if defined(__clang__)
        // Clang cannot allocate eax:edx register pairs but it has sync intrinsics
        storage_type old_expected = expected;
        expected = __sync_val_compare_and_swap(&storage, old_expected, desired);
        return expected == old_expected;
#elif defined(__PIC__)
        // Make sure ebx is saved and restored properly in case
        // of position independent code. To make this work
        // setup register constraints such that ebx can not be
        // used by accident e.g. as base address for the variable
        // to be modified. Accessing "scratch" should always be okay,
        // as it can only be placed on the stack (and therefore
        // accessed through ebp or esp only).
        //
        // In theory, could push/pop ebx onto/off the stack, but movs
        // to a prepared stack slot turn out to be faster.

        uint32_t scratch;
        bool success;
        __asm__ __volatile__
        (
            "movl %%ebx, %[scratch]\n\t"
            "movl %[desired_lo], %%ebx\n\t"
            "lock; cmpxchg8b %[dest]\n\t"
            "movl %[scratch], %%ebx\n\t"
            "sete %[success]"
            : "+A,A,A,A,A,A" (expected), [dest] "+m,m,m,m,m,m" (storage), [scratch] "=m,m,m,m,m,m" (scratch), [success] "=q,m,q,m,q,m" (success)
            : [desired_lo] "S,S,D,D,m,m" ((uint32_t)desired), "c,c,c,c,c,c" ((uint32_t)(desired >> 32))
            : "cc", "memory"
        );
        return success;
#else
        bool success;
        __asm__ __volatile__
        (
            "lock; cmpxchg8b %[dest]\n\t"
            "sete %[success]"
            : "+A,A" (expected), [dest] "+m,m" (storage), [success] "=q,m" (success)
            : "b,b" ((uint32_t)desired), "c,c" ((uint32_t)(desired >> 32))
            : "cc", "memory"
        );
        return success;
#endif
    }

    static BOOST_FORCEINLINE bool compare_exchange_weak(
        storage_type volatile& storage, storage_type& expected, storage_type desired, memory_order success_order, memory_order failure_order) BOOST_NOEXCEPT
    {
        return compare_exchange_strong(storage, expected, desired, success_order, failure_order);
    }

    static BOOST_FORCEINLINE bool is_lock_free(storage_type const volatile&) BOOST_NOEXCEPT
    {
        return true;
    }
};

#endif // defined(BOOST_ATOMIC_DETAIL_X86_HAS_CMPXCHG8B)

#if defined(BOOST_ATOMIC_DETAIL_X86_HAS_CMPXCHG16B)

template< bool Signed >
struct gcc_dcas_x86_64
{
    typedef typename make_storage_type< 16u, Signed >::type storage_type;

    static BOOST_FORCEINLINE void store(storage_type volatile& storage, storage_type v, memory_order) BOOST_NOEXCEPT
    {
        uint64_t const* p_value = (uint64_t const*)&v;
        __asm__ __volatile__
        (
            "movq 0(%[dest]), %%rax\n\t"
            "movq 8(%[dest]), %%rdx\n\t"
            ".align 16\n\t"
            "1: lock; cmpxchg16b 0(%[dest])\n\t"
            "jne 1b"
            :
            : "b" (p_value[0]), "c" (p_value[1]), [dest] "r" (&storage)
            : "cc", "rax", "rdx", "memory"
        );
    }

    static BOOST_FORCEINLINE storage_type load(storage_type const volatile& storage, memory_order) BOOST_NOEXCEPT
    {
#if defined(__clang__)
        // Clang cannot allocate rax:rdx register pairs but it has sync intrinsics
        storage_type value = storage_type();
        return __sync_val_compare_and_swap(&storage, value, value);
#else
        storage_type value;

        // We don't care for comparison result here; the previous value will be stored into value anyway.
        // Also we don't care for rbx and rcx values, they just have to be equal to rax and rdx before cmpxchg16b.
        __asm__ __volatile__
        (
            "movq %%rbx, %%rax\n\t"
            "movq %%rcx, %%rdx\n\t"
            "lock; cmpxchg16b %[storage]"
            : "=&A" (value)
            : [storage] "m" (storage)
            : "cc", "memory"
        );

        return value;
#endif
    }

    static BOOST_FORCEINLINE bool compare_exchange_strong(
        storage_type volatile& storage, storage_type& expected, storage_type desired, memory_order, memory_order) BOOST_NOEXCEPT
    {
#if defined(__clang__)
        // Clang cannot allocate rax:rdx register pairs but it has sync intrinsics
        storage_type old_expected = expected;
        expected = __sync_val_compare_and_swap(&storage, old_expected, desired);
        return expected == old_expected;
#else
        uint64_t const* p_desired = (uint64_t const*)&desired;
        bool success;
        __asm__ __volatile__
        (
            "lock; cmpxchg16b %[dest]\n\t"
            "sete %[success]"
            : "+A,A" (expected), [dest] "+m,m" (storage), [success] "=q,m" (success)
            : "b,b" (p_desired[0]), "c,c" (p_desired[1])
            : "cc", "memory"
        );
        return success;
#endif
    }

    static BOOST_FORCEINLINE bool compare_exchange_weak(
        storage_type volatile& storage, storage_type& expected, storage_type desired, memory_order success_order, memory_order failure_order) BOOST_NOEXCEPT
    {
        return compare_exchange_strong(storage, expected, desired, success_order, failure_order);
    }

    static BOOST_FORCEINLINE bool is_lock_free(storage_type const volatile&) BOOST_NOEXCEPT
    {
        return true;
    }
};

#endif // defined(BOOST_ATOMIC_DETAIL_X86_HAS_CMPXCHG16B)

} // namespace detail
} // namespace atomics
} // namespace boost

#endif // BOOST_ATOMIC_DETAIL_OPS_GCC_X86_DCAS_HPP_INCLUDED_
