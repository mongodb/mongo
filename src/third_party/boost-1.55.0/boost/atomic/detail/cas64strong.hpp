#ifndef BOOST_ATOMIC_DETAIL_CAS64STRONG_HPP
#define BOOST_ATOMIC_DETAIL_CAS64STRONG_HPP

//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  Copyright (c) 2011 Helge Bahmann
//  Copyright (c) 2013 Tim Blechmann

// Build 64-bit atomic operation on integers/UDTs from platform_cmpxchg64_strong
// primitive. It is assumed that 64-bit loads/stores are not
// atomic, so they are implemented through platform_load64/platform_store64.

#include <string.h>
#include <cstddef>
#include <boost/cstdint.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/base.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace atomics {
namespace detail {

/* integral types */

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
        platform_fence_before_store(order);
        platform_store64(v, &v_);
        platform_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = platform_load64(&v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original = load(memory_order_relaxed);
        do {
        } while (!compare_exchange_weak(original, v, order, memory_order_relaxed));
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, success_order, failure_order);
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(success_order);

        bool success = platform_cmpxchg64_strong(expected, desired, &v_);

        if (success) {
            platform_fence_after(success_order);
        } else {
            platform_fence_after(failure_order);
        }

        return success;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original = load(memory_order_relaxed);
        do {
        } while (!compare_exchange_weak(original, original + v, order, memory_order_relaxed));
        return original;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original = load(memory_order_relaxed);
        do {
        } while (!compare_exchange_weak(original, original - v, order, memory_order_relaxed));
        return original;
    }

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original = load(memory_order_relaxed);
        do {
        } while (!compare_exchange_weak(original, original & v, order, memory_order_relaxed));
        return original;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original = load(memory_order_relaxed);
        do {
        } while (!compare_exchange_weak(original, original | v, order, memory_order_relaxed));
        return original;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original = load(memory_order_relaxed);
        do {
        } while (!compare_exchange_weak(original, original ^ v, order, memory_order_relaxed));
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

/* generic types */

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
    store(value_type const& value, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type value_s = 0;
        memcpy(&value_s, &value, sizeof(value_type));
        platform_fence_before_store(order);
        platform_store64(value_s, &v_);
        platform_fence_after_store(order);
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type value_s = platform_load64(&v_);
        platform_fence_after_load(order);
        value_type value;
        memcpy(&value, &value_s, sizeof(value_type));
        return value;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type original = load(memory_order_relaxed);
        do {
        } while (!compare_exchange_weak(original, v, order, memory_order_relaxed));
        return original;
    }

    bool
    compare_exchange_weak(
        value_type & expected,
        value_type const& desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, success_order, failure_order);
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

        platform_fence_before(success_order);
        bool success = platform_cmpxchg64_strong(expected_s, desired_s, &v_);

        if (success) {
            platform_fence_after(success_order);
        } else {
            platform_fence_after(failure_order);
            memcpy(&expected, &expected_s, sizeof(value_type));
        }

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

}
}
}

#endif
