//  Copyright (c) 2011 Helge Bahmann
//  Copyright (c) 2013 Tim Blechmann
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// Use the gnu builtin __sync_val_compare_and_swap to build
// atomic operations for 32 bit and smaller.

#ifndef BOOST_ATOMIC_DETAIL_GENERIC_CAS_HPP
#define BOOST_ATOMIC_DETAIL_GENERIC_CAS_HPP

#include <boost/cstdint.hpp>
#include <boost/atomic/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {

#define BOOST_ATOMIC_THREAD_FENCE 2
inline void
atomic_thread_fence(memory_order order)
{
    switch(order)
    {
    case memory_order_relaxed:
        break;
    case memory_order_release:
    case memory_order_consume:
    case memory_order_acquire:
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        __sync_synchronize();
        break;
    }
}

namespace atomics {
namespace detail {

inline void
platform_fence_before(memory_order)
{
    /* empty, as compare_and_swap is synchronizing already */
}

inline void
platform_fence_after(memory_order)
{
    /* empty, as compare_and_swap is synchronizing already */
}

inline void
platform_fence_before_store(memory_order order)
{
    switch(order)
    {
    case memory_order_relaxed:
    case memory_order_acquire:
    case memory_order_consume:
        break;
    case memory_order_release:
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        __sync_synchronize();
        break;
    }
}

inline void
platform_fence_after_store(memory_order order)
{
    if (order == memory_order_seq_cst)
        __sync_synchronize();
}

inline void
platform_fence_after_load(memory_order order)
{
    switch(order)
    {
    case memory_order_relaxed:
    case memory_order_release:
        break;
    case memory_order_consume:
    case memory_order_acquire:
    case memory_order_acq_rel:
    case memory_order_seq_cst:
        __sync_synchronize();
        break;
    }
}

template<typename T>
inline bool
platform_cmpxchg32_strong(T & expected, T desired, volatile T * ptr)
{
    T found = __sync_val_compare_and_swap(ptr, expected, desired);
    bool success = (found == expected);
    expected = found;
    return success;
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
}
}

#include <boost/atomic/detail/base.hpp>

#if !defined(BOOST_ATOMIC_FORCE_FALLBACK)

#define BOOST_ATOMIC_CHAR_LOCK_FREE 2
#define BOOST_ATOMIC_SHORT_LOCK_FREE 2
#define BOOST_ATOMIC_INT_LOCK_FREE 2
#define BOOST_ATOMIC_LONG_LOCK_FREE (sizeof(long) <= 4 ? 2 : 0)
#define BOOST_ATOMIC_LLONG_LOCK_FREE (sizeof(long long) <= 4 ? 2 : 0)
#define BOOST_ATOMIC_POINTER_LOCK_FREE (sizeof(void *) <= 4 ? 2 : 0)
#define BOOST_ATOMIC_BOOL_LOCK_FREE 2

#include <boost/atomic/detail/cas32strong.hpp>

#endif /* !defined(BOOST_ATOMIC_FORCE_FALLBACK) */

#endif
