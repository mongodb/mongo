#ifndef BOOST_ATOMIC_DETAIL_LINUX_ARM_HPP
#define BOOST_ATOMIC_DETAIL_LINUX_ARM_HPP

//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  Copyright (c) 2009, 2011 Helge Bahmann
//  Copyright (c) 2009 Phil Endecott
//  Copyright (c) 2013 Tim Blechmann
//  Linux-specific code by Phil Endecott

// Different ARM processors have different atomic instructions.  In particular,
// architecture versions before v6 (which are still in widespread use, e.g. the
// Intel/Marvell XScale chips like the one in the NSLU2) have only atomic swap.
// On Linux the kernel provides some support that lets us abstract away from
// these differences: it provides emulated CAS and barrier functions at special
// addresses that are guaranteed not to be interrupted by the kernel.  Using
// this facility is slightly slower than inline assembler would be, but much
// faster than a system call.
//
// While this emulated CAS is "strong" in the sense that it does not fail
// "spuriously" (i.e.: it never fails to perform the exchange when the value
// found equals the value expected), it does not return the found value on
// failure. To satisfy the atomic API, compare_exchange_{weak|strong} must
// return the found value on failure, and we have to manually load this value
// after the emulated CAS reports failure. This in turn introduces a race
// between the CAS failing (due to the "wrong" value being found) and subsequently
// loading (which might turn up the "right" value). From an application's
// point of view this looks like "spurious failure", and therefore the
// emulated CAS is only good enough to provide compare_exchange_weak
// semantics.

#include <cstddef>
#include <boost/cstdint.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace atomics {
namespace detail {

inline void
arm_barrier(void)
{
    void (*kernel_dmb)(void) = (void (*)(void)) 0xffff0fa0;
    kernel_dmb();
}

inline void
platform_fence_before(memory_order order)
{
    switch(order) {
        case memory_order_release:
        case memory_order_acq_rel:
        case memory_order_seq_cst:
            arm_barrier();
        case memory_order_consume:
        default:;
    }
}

inline void
platform_fence_after(memory_order order)
{
    switch(order) {
        case memory_order_acquire:
        case memory_order_acq_rel:
        case memory_order_seq_cst:
            arm_barrier();
        default:;
    }
}

inline void
platform_fence_before_store(memory_order order)
{
    platform_fence_before(order);
}

inline void
platform_fence_after_store(memory_order order)
{
    if (order == memory_order_seq_cst)
        arm_barrier();
}

inline void
platform_fence_after_load(memory_order order)
{
    platform_fence_after(order);
}

template<typename T>
inline bool
platform_cmpxchg32(T & expected, T desired, volatile T * ptr)
{
    typedef T (*kernel_cmpxchg32_t)(T oldval, T newval, volatile T * ptr);

    if (((kernel_cmpxchg32_t) 0xffff0fc0)(expected, desired, ptr) == 0) {
        return true;
    } else {
        expected = *ptr;
        return false;
    }
}

}
}

#define BOOST_ATOMIC_THREAD_FENCE 2
inline void
atomic_thread_fence(memory_order order)
{
    switch(order) {
        case memory_order_acquire:
        case memory_order_release:
        case memory_order_acq_rel:
        case memory_order_seq_cst:
            atomics::detail::arm_barrier();
        default:;
    }
}

#define BOOST_ATOMIC_SIGNAL_FENCE 2
inline void
atomic_signal_fence(memory_order)
{
    __asm__ __volatile__ ("" ::: "memory");
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
        atomics::detail::platform_fence_before_store(order);
        const_cast<volatile uint32_t &>(v_) = 0;
        atomics::detail::platform_fence_after_store(order);
    }

    bool
    test_and_set(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        atomics::detail::platform_fence_before(order);
        uint32_t expected = v_;
        do {
            if (expected == 1)
                break;
        } while (!atomics::detail::platform_cmpxchg32(expected, (uint32_t)1, &v_));
        atomics::detail::platform_fence_after(order);
        return expected;
    }
};

#define BOOST_ATOMIC_FLAG_LOCK_FREE 2

}

#include <boost/atomic/detail/base.hpp>

#if !defined(BOOST_ATOMIC_FORCE_FALLBACK)

#define BOOST_ATOMIC_CHAR_LOCK_FREE 2
#define BOOST_ATOMIC_CHAR16_T_LOCK_FREE 2
#define BOOST_ATOMIC_CHAR32_T_LOCK_FREE 2
#define BOOST_ATOMIC_WCHAR_T_LOCK_FREE 2
#define BOOST_ATOMIC_SHORT_LOCK_FREE 2
#define BOOST_ATOMIC_INT_LOCK_FREE 2
#define BOOST_ATOMIC_LONG_LOCK_FREE 2
#define BOOST_ATOMIC_LLONG_LOCK_FREE 0
#define BOOST_ATOMIC_POINTER_LOCK_FREE 2
#define BOOST_ATOMIC_BOOL_LOCK_FREE 2

#include <boost/atomic/detail/cas32weak.hpp>

#endif /* !defined(BOOST_ATOMIC_FORCE_FALLBACK) */

#endif
