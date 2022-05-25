/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2021 Andrey Semashev
 */
/*!
 * \file   atomic/detail/wait_ops_darwin_ulock.hpp
 *
 * This header contains implementation of the waiting/notifying atomic operations based on Darwin systems using ulock syscalls.
 *
 * https://github.com/apple/darwin-xnu/blob/master/bsd/sys/ulock.h
 * https://github.com/apple/darwin-xnu/blob/master/bsd/kern/sys_ulock.c
 */

#ifndef BOOST_ATOMIC_DETAIL_WAIT_OPS_DARWIN_ULOCK_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_WAIT_OPS_DARWIN_ULOCK_HPP_INCLUDED_

#include <stdint.h>
#include <cerrno>
#include <boost/memory_order.hpp>
#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/wait_capabilities.hpp>
#include <boost/atomic/detail/wait_operations_fwd.hpp>
#include <boost/atomic/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace atomics {
namespace detail {

extern "C" {
// Timeout is in microseconds with zero meaning no timeout
int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout);
#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_WAIT2)
// Timeout is in nanoseconds with zero meaning no timeout
int __ulock_wait2(uint32_t operation, void* addr, uint64_t value, uint64_t timeout, uint64_t value2);
#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_WAIT2)
int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
} // extern "C"

enum ulock_op
{
    ulock_op_compare_and_wait = 1,
#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)
    ulock_op_compare_and_wait_shared = 3,
#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)
#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK64)
    ulock_op_compare_and_wait64 = 5,
#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)
    ulock_op_compare_and_wait64_shared = 6,
#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)
#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK64)

    // Flags for __ulock_wake
    ulock_flag_wake_all = 0x00000100,

    // Generic flags
    ulock_flag_no_errno = 0x01000000
};

template< typename Base, uint32_t Opcode >
struct wait_operations_darwin_ulock_common :
    public Base
{
    typedef Base base_type;
    typedef typename base_type::storage_type storage_type;

    static BOOST_CONSTEXPR_OR_CONST bool always_has_native_wait_notify = true;

    static BOOST_FORCEINLINE bool has_native_wait_notify(storage_type const volatile&) BOOST_NOEXCEPT
    {
        return true;
    }

    static BOOST_FORCEINLINE storage_type wait(storage_type const volatile& storage, storage_type old_val, memory_order order) BOOST_NOEXCEPT
    {
        storage_type new_val = base_type::load(storage, order);
        while (new_val == old_val)
        {
#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_WAIT2)
            __ulock_wait2(Opcode | ulock_flag_no_errno, const_cast< storage_type* >(&storage), old_val, 0u, 0u);
#else
            __ulock_wait(Opcode | ulock_flag_no_errno, const_cast< storage_type* >(&storage), old_val, 0u);
#endif
            new_val = base_type::load(storage, order);
        }

        return new_val;
    }

    static BOOST_FORCEINLINE void notify_one(storage_type volatile& storage) BOOST_NOEXCEPT
    {
        while (true)
        {
            const int res = __ulock_wake(Opcode | ulock_flag_no_errno, const_cast< storage_type* >(&storage), 0u);
            if (BOOST_LIKELY(res != -EINTR))
                break;
        }
    }

    static BOOST_FORCEINLINE void notify_all(storage_type volatile& storage) BOOST_NOEXCEPT
    {
        while (true)
        {
            const int res = __ulock_wake(Opcode | ulock_flag_wake_all | ulock_flag_no_errno, const_cast< storage_type* >(&storage), 0u);
            if (BOOST_LIKELY(res != -EINTR))
                break;
        }
    }
};

template< typename Base >
struct wait_operations< Base, sizeof(uint32_t), true, false > :
    public wait_operations_darwin_ulock_common< Base, ulock_op_compare_and_wait >
{
};

#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)

template< typename Base >
struct wait_operations< Base, sizeof(uint32_t), true, true > :
    public wait_operations_darwin_ulock_common< Base, ulock_op_compare_and_wait_shared >
{
};

#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)

#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK64)

template< typename Base >
struct wait_operations< Base, sizeof(uint64_t), true, false > :
    public wait_operations_darwin_ulock_common< Base, ulock_op_compare_and_wait64 >
{
};

#if defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)

template< typename Base >
struct wait_operations< Base, sizeof(uint64_t), true, true > :
    public wait_operations_darwin_ulock_common< Base, ulock_op_compare_and_wait64_shared >
{
};

#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK_SHARED)
#endif // defined(BOOST_ATOMIC_DETAIL_HAS_DARWIN_ULOCK64)

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>

#endif // BOOST_ATOMIC_DETAIL_WAIT_OPS_DARWIN_ULOCK_HPP_INCLUDED_
