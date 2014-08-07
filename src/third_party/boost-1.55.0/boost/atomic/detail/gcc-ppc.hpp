#ifndef BOOST_ATOMIC_DETAIL_GCC_PPC_HPP
#define BOOST_ATOMIC_DETAIL_GCC_PPC_HPP

//  Copyright (c) 2009 Helge Bahmann
//  Copyright (c) 2013 Tim Blechmann
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <string.h>
#include <cstddef>
#include <boost/cstdint.hpp>
#include <boost/atomic/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

/*
    Refer to: Motorola: "Programming Environments Manual for 32-Bit
    Implementations of the PowerPC Architecture", Appendix E:
    "Synchronization Programming Examples" for an explanation of what is
    going on here (can be found on the web at various places by the
    name "MPCFPE32B.pdf", Google is your friend...)

    Most of the atomic operations map to instructions in a relatively
    straight-forward fashion, but "load"s may at first glance appear
    a bit strange as they map to:

            lwz %rX, addr
            cmpw %rX, %rX
            bne- 1f
        1:

    That is, the CPU is forced to perform a branch that "formally" depends
    on the value retrieved from memory. This scheme has an overhead of
    about 1-2 clock cycles per load, but it allows to map "acquire" to
    the "isync" instruction instead of "sync" uniformly and for all type
    of atomic operations. Since "isync" has a cost of about 15 clock
    cycles, while "sync" hast a cost of about 50 clock cycles, the small
    penalty to atomic loads more than compensates for this.

    Byte- and halfword-sized atomic values are realized by encoding the
    value to be represented into a word, performing sign/zero extension
    as appropriate. This means that after add/sub operations the value
    needs fixing up to accurately preserve the wrap-around semantic of
    the smaller type. (Nothing special needs to be done for the bit-wise
    and the "exchange type" operators as the compiler already sees to
    it that values carried in registers are extended appropriately and
    everything falls into place naturally).

    The register constraint "b"  instructs gcc to use any register
    except r0; this is sometimes required because the encoding for
    r0 is used to signify "constant zero" in a number of instructions,
    making r0 unusable in this place. For simplicity this constraint
    is used everywhere since I am to lazy to look this up on a
    per-instruction basis, and ppc has enough registers for this not
    to pose a problem.
*/

namespace boost {
namespace atomics {
namespace detail {

inline void
ppc_fence_before(memory_order order)
{
    switch(order)
    {
    case memory_order_release:
    case memory_order_acq_rel:
#if defined(__powerpc64__)
        __asm__ __volatile__ ("lwsync" ::: "memory");
        break;
#endif
    case memory_order_seq_cst:
        __asm__ __volatile__ ("sync" ::: "memory");
    default:;
    }
}

inline void
ppc_fence_after(memory_order order)
{
    switch(order)
    {
    case memory_order_acquire:
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        __asm__ __volatile__ ("isync");
    case memory_order_consume:
        __asm__ __volatile__ ("" ::: "memory");
    default:;
    }
}

inline void
ppc_fence_after_store(memory_order order)
{
    switch(order)
    {
    case memory_order_seq_cst:
        __asm__ __volatile__ ("sync");
    default:;
    }
}

}
}

class atomic_flag
{
private:
    atomic_flag(const atomic_flag &) /* = delete */ ;
    atomic_flag & operator=(const atomic_flag &) /* = delete */ ;
    uint32_t v_;
public:
    BOOST_CONSTEXPR atomic_flag(void) BOOST_NOEXCEPT : v_(0) {}

    void
    clear(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        atomics::detail::ppc_fence_before(order);
        const_cast<volatile uint32_t &>(v_) = 0;
        atomics::detail::ppc_fence_after_store(order);
    }

    bool
    test_and_set(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        uint32_t original;
        atomics::detail::ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (1)
            : "cr0"
        );
        atomics::detail::ppc_fence_after(order);
        return original;
    }
};

} /* namespace boost */

#define BOOST_ATOMIC_FLAG_LOCK_FREE 2

#include <boost/atomic/detail/base.hpp>

#if !defined(BOOST_ATOMIC_FORCE_FALLBACK)

#define BOOST_ATOMIC_CHAR_LOCK_FREE 2
#define BOOST_ATOMIC_CHAR16_T_LOCK_FREE 2
#define BOOST_ATOMIC_CHAR32_T_LOCK_FREE 2
#define BOOST_ATOMIC_WCHAR_T_LOCK_FREE 2
#define BOOST_ATOMIC_SHORT_LOCK_FREE 2
#define BOOST_ATOMIC_INT_LOCK_FREE 2
#define BOOST_ATOMIC_LONG_LOCK_FREE 2
#define BOOST_ATOMIC_POINTER_LOCK_FREE 2
#if defined(__powerpc64__)
#define BOOST_ATOMIC_LLONG_LOCK_FREE 2
#else
#define BOOST_ATOMIC_LLONG_LOCK_FREE 0
#endif
#define BOOST_ATOMIC_BOOL_LOCK_FREE 2

/* Would like to move the slow-path of failed compare_exchange
(that clears the "success" bit) out-of-line. gcc can in
principle do that using ".subsection"/".previous", but Apple's
binutils seemingly does not understand that. Therefore wrap
the "clear" of the flag in a macro and let it remain
in-line for Apple
*/

#if !defined(__APPLE__)

#define BOOST_ATOMIC_ASM_SLOWPATH_CLEAR \
    "9:\n" \
    ".subsection 2\n" \
    "2: addi %1,0,0\n" \
    "b 9b\n" \
    ".previous\n" \

#else

#define BOOST_ATOMIC_ASM_SLOWPATH_CLEAR \
    "b 9f\n" \
    "2: addi %1,0,0\n" \
    "9:\n" \

#endif

namespace boost {
namespace atomics {
namespace detail {

/* integral types */

template<typename T>
class base_atomic<T, int, 1, true>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef int32_t storage_type;
    typedef T difference_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m"(v_)
            : "r" (v)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v;
        __asm__ __volatile__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=&r" (v)
            : "m" (v_)
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"
            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "add %1,%0,%3\n"
            "extsb %1, %1\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "extsb %1, %1\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "and %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "or %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "xor %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_INTEGRAL_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    storage_type v_;
};

template<typename T>
class base_atomic<T, int, 1, false>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef uint32_t storage_type;
    typedef T difference_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m"(v_)
            : "r" (v)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v;
        __asm__ __volatile__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=&r" (v)
            : "m" (v_)
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "add %1,%0,%3\n"
            "rlwinm %1, %1, 0, 0xff\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "rlwinm %1, %1, 0, 0xff\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "and %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "or %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "xor %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_INTEGRAL_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    storage_type v_;
};

template<typename T>
class base_atomic<T, int, 2, true>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef int32_t storage_type;
    typedef T difference_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m"(v_)
            : "r" (v)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v;
        __asm__ __volatile__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=&r" (v)
            : "m" (v_)
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "add %1,%0,%3\n"
            "extsh %1, %1\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "extsh %1, %1\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "and %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "or %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "xor %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_INTEGRAL_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    storage_type v_;
};

template<typename T>
class base_atomic<T, int, 2, false>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef uint32_t storage_type;
    typedef T difference_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m"(v_)
            : "r" (v)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v;
        __asm__ __volatile__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=&r" (v)
            : "m" (v_)
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "add %1,%0,%3\n"
            "rlwinm %1, %1, 0, 0xffff\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "rlwinm %1, %1, 0, 0xffff\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "and %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "or %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "xor %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_INTEGRAL_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    storage_type v_;
};

template<typename T, bool Sign>
class base_atomic<T, int, 4, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef T difference_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        const_cast<volatile value_type &>(v_) = v;
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        __asm__ __volatile__ (
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "+b"(v)
            :
            : "cr0"
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "add %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "and %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "or %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "xor %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_INTEGRAL_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    value_type v_;
};

#if defined(__powerpc64__)

template<typename T, bool Sign>
class base_atomic<T, int, 8, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef T difference_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        const_cast<volatile value_type &>(v_) = v;
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        __asm__ __volatile__ (
            "cmpd %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "+b"(v)
            :
            : "cr0"
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y1\n"
            "stdcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "ldarx %0,%y2\n"
            "cmpd %0, %3\n"
            "bne- 2f\n"
            "stdcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: ldarx %0,%y2\n"
            "cmpd %0, %3\n"
            "bne- 2f\n"
            "stdcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "add %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "and %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "or %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "xor %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_INTEGRAL_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    value_type v_;
};

#endif

/* pointer types */

#if !defined(__powerpc64__)

template<bool Sign>
class base_atomic<void *, void *, 4, Sign>
{
private:
    typedef base_atomic this_type;
    typedef std::ptrdiff_t difference_type;
    typedef void * value_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m" (v_)
            : "r" (v)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v;
        __asm__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=r"(v)
            : "m"(v_)
            : "cr0"
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    value_type
    fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "add %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    BOOST_ATOMIC_DECLARE_VOID_POINTER_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    value_type v_;
};

template<typename T, bool Sign>
class base_atomic<T *, void *, 4, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T * value_type;
    typedef std::ptrdiff_t difference_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m" (v_)
            : "r" (v)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v;
        __asm__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=r"(v)
            : "m"(v_)
            : "cr0"
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    value_type
    fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        v = v * sizeof(*v_);
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "add %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        v = v * sizeof(*v_);
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "stwcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_POINTER_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    value_type v_;
};

#else

template<bool Sign>
class base_atomic<void *, void *, 8, Sign>
{
private:
    typedef base_atomic this_type;
    typedef std::ptrdiff_t difference_type;
    typedef void * value_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        __asm__ (
            "std %1, %0\n"
            : "+m" (v_)
            : "r" (v)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v;
        __asm__ (
            "ld %0, %1\n"
            "cmpd %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=r"(v)
            : "m"(v_)
            : "cr0"
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y1\n"
            "stdcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "ldarx %0,%y2\n"
            "cmpd %0, %3\n"
            "bne- 2f\n"
            "stdcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: ldarx %0,%y2\n"
            "cmpd %0, %3\n"
            "bne- 2f\n"
            "stdcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    value_type
    fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "add %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    BOOST_ATOMIC_DECLARE_VOID_POINTER_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    value_type v_;
};

template<typename T, bool Sign>
class base_atomic<T *, void *, 8, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T * value_type;
    typedef std::ptrdiff_t difference_type;

protected:
    typedef value_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type v) BOOST_NOEXCEPT : v_(v) {}

    void
    store(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        ppc_fence_before(order);
        __asm__ (
            "std %1, %0\n"
            : "+m" (v_)
            : "r" (v)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v;
        __asm__ (
            "ld %0, %1\n"
            "cmpd %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=r"(v)
            : "m"(v_)
            : "cr0"
        );
        ppc_fence_after(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y1\n"
            "stdcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (v)
            : "cr0"
        );
        ppc_fence_after(order);
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "ldarx %0,%y2\n"
            "cmpd %0, %3\n"
            "bne- 2f\n"
            "stdcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: ldarx %0,%y2\n"
            "cmpd %0, %3\n"
            "bne- 2f\n"
            "stdcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected), "=&b" (success), "+Z"(v_)
            : "b" (expected), "b" (desired)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        return success;
    }

    value_type
    fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        v = v * sizeof(*v_);
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "add %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    value_type
    fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        v = v * sizeof(*v_);
        value_type original, tmp;
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y2\n"
            "sub %1,%0,%3\n"
            "stdcx. %1,%y2\n"
            "bne- 1b\n"
            : "=&b" (original), "=&b" (tmp), "+Z"(v_)
            : "b" (v)
            : "cc");
        ppc_fence_after(order);
        return original;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_POINTER_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    value_type v_;
};

#endif

/* generic */

template<typename T, bool Sign>
class base_atomic<T, void, 1, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef uint32_t storage_type;

protected:
    typedef value_type const& value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    explicit base_atomic(value_type const& v) BOOST_NOEXCEPT : v_(0)
    {
        memcpy(&v_, &v, sizeof(value_type));
    }

    void
    store(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0;
        memcpy(&tmp, &v, sizeof(value_type));
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m" (v_)
            : "r" (tmp)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type tmp;
        __asm__ __volatile__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=r"(tmp)
            : "m"(v_)
            : "cr0"
        );
        ppc_fence_after(order);

        value_type v;
        memcpy(&v, &tmp, sizeof(value_type));
        return v;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0, original;
        memcpy(&tmp, &v, sizeof(value_type));
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (tmp)
            : "cr0"
        );
        ppc_fence_after(order);
        value_type res;
        memcpy(&res, &original, sizeof(value_type));
        return res;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        storage_type expected_s = 0, desired_s = 0;
        memcpy(&expected_s, &expected, sizeof(value_type));
        memcpy(&desired_s, &desired, sizeof(value_type));

        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected_s), "=&b" (success), "+Z"(v_)
            : "b" (expected_s), "b" (desired_s)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        memcpy(&expected, &expected_s, sizeof(value_type));
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        storage_type expected_s = 0, desired_s = 0;
        memcpy(&expected_s, &expected, sizeof(value_type));
        memcpy(&desired_s, &desired, sizeof(value_type));

        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected_s), "=&b" (success), "+Z"(v_)
            : "b" (expected_s), "b" (desired_s)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        memcpy(&expected, &expected_s, sizeof(value_type));
        return success;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_BASE_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    storage_type v_;
};

template<typename T, bool Sign>
class base_atomic<T, void, 2, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef uint32_t storage_type;

protected:
    typedef value_type const& value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    explicit base_atomic(value_type const& v) BOOST_NOEXCEPT : v_(0)
    {
        memcpy(&v_, &v, sizeof(value_type));
    }

    void
    store(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0;
        memcpy(&tmp, &v, sizeof(value_type));
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m" (v_)
            : "r" (tmp)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type tmp;
        __asm__ __volatile__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=r"(tmp)
            : "m"(v_)
            : "cr0"
        );
        ppc_fence_after(order);

        value_type v;
        memcpy(&v, &tmp, sizeof(value_type));
        return v;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0, original;
        memcpy(&tmp, &v, sizeof(value_type));
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (tmp)
            : "cr0"
        );
        ppc_fence_after(order);
        value_type res;
        memcpy(&res, &original, sizeof(value_type));
        return res;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        storage_type expected_s = 0, desired_s = 0;
        memcpy(&expected_s, &expected, sizeof(value_type));
        memcpy(&desired_s, &desired, sizeof(value_type));

        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected_s), "=&b" (success), "+Z"(v_)
            : "b" (expected_s), "b" (desired_s)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        memcpy(&expected, &expected_s, sizeof(value_type));
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        storage_type expected_s = 0, desired_s = 0;
        memcpy(&expected_s, &expected, sizeof(value_type));
        memcpy(&desired_s, &desired, sizeof(value_type));

        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected_s), "=&b" (success), "+Z"(v_)
            : "b" (expected_s), "b" (desired_s)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        memcpy(&expected, &expected_s, sizeof(value_type));
        return success;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_BASE_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    storage_type v_;
};

template<typename T, bool Sign>
class base_atomic<T, void, 4, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef uint32_t storage_type;

protected:
    typedef value_type const& value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    explicit base_atomic(value_type const& v) BOOST_NOEXCEPT : v_(0)
    {
        memcpy(&v_, &v, sizeof(value_type));
    }

    void
    store(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0;
        memcpy(&tmp, &v, sizeof(value_type));
        ppc_fence_before(order);
        __asm__ (
            "stw %1, %0\n"
            : "+m" (v_)
            : "r" (tmp)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type tmp;
        __asm__ __volatile__ (
            "lwz %0, %1\n"
            "cmpw %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=r"(tmp)
            : "m"(v_)
            : "cr0"
        );
        ppc_fence_after(order);

        value_type v;
        memcpy(&v, &tmp, sizeof(value_type));
        return v;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0, original;
        memcpy(&tmp, &v, sizeof(value_type));
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "lwarx %0,%y1\n"
            "stwcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (tmp)
            : "cr0"
        );
        ppc_fence_after(order);
        value_type res;
        memcpy(&res, &original, sizeof(value_type));
        return res;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        storage_type expected_s = 0, desired_s = 0;
        memcpy(&expected_s, &expected, sizeof(value_type));
        memcpy(&desired_s, &desired, sizeof(value_type));

        int success;
        ppc_fence_before(success_order);
        __asm__(
            "lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected_s), "=&b" (success), "+Z"(v_)
            : "b" (expected_s), "b" (desired_s)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        memcpy(&expected, &expected_s, sizeof(value_type));
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        storage_type expected_s = 0, desired_s = 0;
        memcpy(&expected_s, &expected, sizeof(value_type));
        memcpy(&desired_s, &desired, sizeof(value_type));

        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: lwarx %0,%y2\n"
            "cmpw %0, %3\n"
            "bne- 2f\n"
            "stwcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected_s), "=&b" (success), "+Z"(v_)
            : "b" (expected_s), "b" (desired_s)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        memcpy(&expected, &expected_s, sizeof(value_type));
        return success;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_BASE_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    storage_type v_;
};

#if defined(__powerpc64__)

template<typename T, bool Sign>
class base_atomic<T, void, 8, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef uint64_t storage_type;

protected:
    typedef value_type const& value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    explicit base_atomic(value_type const& v) BOOST_NOEXCEPT : v_(0)
    {
        memcpy(&v_, &v, sizeof(value_type));
    }

    void
    store(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp;
        memcpy(&tmp, &v, sizeof(value_type));
        ppc_fence_before(order);
        __asm__ (
            "std %1, %0\n"
            : "+m" (v_)
            : "r" (tmp)
        );
        ppc_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type tmp;
        __asm__ __volatile__ (
            "ld %0, %1\n"
            "cmpd %0, %0\n"
            "bne- 1f\n"
            "1:\n"
            : "=r"(tmp)
            : "m"(v_)
            : "cr0"
        );
        ppc_fence_after(order);

        value_type v;
        memcpy(&v, &tmp, sizeof(value_type));
        return v;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0, original;
        memcpy(&tmp, &v, sizeof(value_type));
        ppc_fence_before(order);
        __asm__ (
            "1:\n"
            "ldarx %0,%y1\n"
            "stdcx. %2,%y1\n"
            "bne- 1b\n"
            : "=&b" (original), "+Z"(v_)
            : "b" (tmp)
            : "cr0"
        );
        ppc_fence_after(order);
        value_type res;
        memcpy(&res, &original, sizeof(value_type));
        return res;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        storage_type expected_s, desired_s;
        memcpy(&expected_s, &expected, sizeof(value_type));
        memcpy(&desired_s, &desired, sizeof(value_type));

        int success;
        ppc_fence_before(success_order);
        __asm__(
            "ldarx %0,%y2\n"
            "cmpd %0, %3\n"
            "bne- 2f\n"
            "stdcx. %4,%y2\n"
            "bne- 2f\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected_s), "=&b" (success), "+Z"(v_)
            : "b" (expected_s), "b" (desired_s)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        memcpy(&expected, &expected_s, sizeof(value_type));
        return success;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        storage_type expected_s, desired_s;
        memcpy(&expected_s, &expected, sizeof(value_type));
        memcpy(&desired_s, &desired, sizeof(value_type));

        int success;
        ppc_fence_before(success_order);
        __asm__(
            "0: ldarx %0,%y2\n"
            "cmpd %0, %3\n"
            "bne- 2f\n"
            "stdcx. %4,%y2\n"
            "bne- 0b\n"
            "addi %1,0,1\n"
            "1:"

            BOOST_ATOMIC_ASM_SLOWPATH_CLEAR
            : "=&b" (expected_s), "=&b" (success), "+Z"(v_)
            : "b" (expected_s), "b" (desired_s)
            : "cr0"
        );
        if (success)
            ppc_fence_after(success_order);
        else
            ppc_fence_after(failure_order);
        memcpy(&expected, &expected_s, sizeof(value_type));
        return success;
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    BOOST_ATOMIC_DECLARE_BASE_OPERATORS

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    storage_type v_;
};

#endif

}
}

#define BOOST_ATOMIC_THREAD_FENCE 2
inline void
atomic_thread_fence(memory_order order)
{
    switch(order)
    {
    case memory_order_acquire:
        __asm__ __volatile__ ("isync" ::: "memory");
        break;
    case memory_order_release:
#if defined(__powerpc64__)
        __asm__ __volatile__ ("lwsync" ::: "memory");
        break;
#endif
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        __asm__ __volatile__ ("sync" ::: "memory");
    default:;
    }
}

#define BOOST_ATOMIC_SIGNAL_FENCE 2
inline void
atomic_signal_fence(memory_order order)
{
    switch(order)
    {
    case memory_order_acquire:
    case memory_order_release:
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        __asm__ __volatile__ ("" ::: "memory");
        break;
    default:;
    }
}

}

#endif /* !defined(BOOST_ATOMIC_FORCE_FALLBACK) */

#endif
