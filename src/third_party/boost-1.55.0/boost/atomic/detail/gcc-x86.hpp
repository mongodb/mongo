#ifndef BOOST_ATOMIC_DETAIL_GCC_X86_HPP
#define BOOST_ATOMIC_DETAIL_GCC_X86_HPP

//  Copyright (c) 2009 Helge Bahmann
//  Copyright (c) 2012 Tim Blechmann
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

namespace boost {
namespace atomics {
namespace detail {

#if defined(__x86_64__) || defined(__SSE2__)
# define BOOST_ATOMIC_X86_FENCE_INSTR "mfence\n"
#else
# define BOOST_ATOMIC_X86_FENCE_INSTR "lock ; addl $0, (%%esp)\n"
#endif

#define BOOST_ATOMIC_X86_PAUSE() __asm__ __volatile__ ("pause\n")

#if defined(__i386__) &&\
    (\
        defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8) ||\
        defined(__i586__) || defined(__i686__) || defined(__pentium4__) || defined(__nocona__) || defined(__core2__) || defined(__corei7__) ||\
        defined(__k6__) || defined(__athlon__) || defined(__k8__) || defined(__amdfam10__) || defined(__bdver1__) || defined(__bdver2__) || defined(__bdver3__) || defined(__btver1__) || defined(__btver2__)\
    )
#define BOOST_ATOMIC_X86_HAS_CMPXCHG8B 1
#endif

#if defined(__x86_64__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
#define BOOST_ATOMIC_X86_HAS_CMPXCHG16B 1
#endif

inline void
platform_fence_before(memory_order order)
{
    switch(order)
    {
    case memory_order_relaxed:
    case memory_order_acquire:
    case memory_order_consume:
        break;
    case memory_order_release:
    case memory_order_acq_rel:
        __asm__ __volatile__ ("" ::: "memory");
        /* release */
        break;
    case memory_order_seq_cst:
        __asm__ __volatile__ ("" ::: "memory");
        /* seq */
        break;
    default:;
    }
}

inline void
platform_fence_after(memory_order order)
{
    switch(order)
    {
    case memory_order_relaxed:
    case memory_order_release:
        break;
    case memory_order_acquire:
    case memory_order_acq_rel:
        __asm__ __volatile__ ("" ::: "memory");
        /* acquire */
        break;
    case memory_order_consume:
        /* consume */
        break;
    case memory_order_seq_cst:
        __asm__ __volatile__ ("" ::: "memory");
        /* seq */
        break;
    default:;
    }
}

inline void
platform_fence_after_load(memory_order order)
{
    switch(order)
    {
    case memory_order_relaxed:
    case memory_order_release:
        break;
    case memory_order_acquire:
    case memory_order_acq_rel:
        __asm__ __volatile__ ("" ::: "memory");
        break;
    case memory_order_consume:
        break;
    case memory_order_seq_cst:
        __asm__ __volatile__ ("" ::: "memory");
        break;
    default:;
    }
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
        __asm__ __volatile__ ("" ::: "memory");
        /* release */
        break;
    case memory_order_seq_cst:
        __asm__ __volatile__ ("" ::: "memory");
        /* seq */
        break;
    default:;
    }
}

inline void
platform_fence_after_store(memory_order order)
{
    switch(order)
    {
    case memory_order_relaxed:
    case memory_order_release:
        break;
    case memory_order_acquire:
    case memory_order_acq_rel:
        __asm__ __volatile__ ("" ::: "memory");
        /* acquire */
        break;
    case memory_order_consume:
        /* consume */
        break;
    case memory_order_seq_cst:
        __asm__ __volatile__ ("" ::: "memory");
        /* seq */
        break;
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

    bool
    test_and_set(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        uint32_t v = 1;
        atomics::detail::platform_fence_before(order);
        __asm__ __volatile__ (
            "xchgl %0, %1"
            : "+r" (v), "+m" (v_)
        );
        atomics::detail::platform_fence_after(order);
        return v;
    }

    void
    clear(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        if (order == memory_order_seq_cst) {
            uint32_t v = 0;
            __asm__ __volatile__ (
                "xchgl %0, %1"
                : "+r" (v), "+m" (v_)
            );
        } else {
            atomics::detail::platform_fence_before(order);
            v_ = 0;
        }
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

#if defined(__x86_64__) || defined(BOOST_ATOMIC_X86_HAS_CMPXCHG8B)
#define BOOST_ATOMIC_LLONG_LOCK_FREE 2
#endif

#if defined(BOOST_ATOMIC_X86_HAS_CMPXCHG16B) && (defined(BOOST_HAS_INT128) || !defined(BOOST_NO_ALIGNMENT))
#define BOOST_ATOMIC_INT128_LOCK_FREE 2
#endif

#define BOOST_ATOMIC_POINTER_LOCK_FREE 2
#define BOOST_ATOMIC_BOOL_LOCK_FREE 2

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
        __asm__ __volatile__ ("" ::: "memory");
        break;
    case memory_order_acquire:
        __asm__ __volatile__ ("" ::: "memory");
        break;
    case memory_order_acq_rel:
        __asm__ __volatile__ ("" ::: "memory");
        break;
    case memory_order_consume:
        break;
    case memory_order_seq_cst:
        __asm__ __volatile__ (BOOST_ATOMIC_X86_FENCE_INSTR ::: "memory");
        break;
    default:;
    }
}

#define BOOST_ATOMIC_SIGNAL_FENCE 2
inline void
atomic_signal_fence(memory_order)
{
    __asm__ __volatile__ ("" ::: "memory");
}

namespace atomics {
namespace detail {

template<typename T, bool Sign>
class base_atomic<T, int, 1, Sign>
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
        if (order != memory_order_seq_cst) {
            platform_fence_before(order);
            const_cast<volatile value_type &>(v_) = v;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "lock ; xaddb %0, %1"
            : "+q" (v), "+m" (v_)
            :
            : "cc"
        );
        platform_fence_after(order);
        return v;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fetch_add(-v, order);
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgb %0, %1"
            : "+q" (v), "+m" (v_)
        );
        platform_fence_after(order);
        return v;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        value_type previous = expected;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgb %3, %1\n\t"
            "sete %2"
            : "+a" (previous), "+m" (v_), "=q" (success)
            : "q" (desired)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        expected = previous;
        return success;
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

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp & v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp | v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp ^ v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
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

template<typename T, bool Sign>
class base_atomic<T, int, 2, Sign>
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
        if (order != memory_order_seq_cst) {
            platform_fence_before(order);
            const_cast<volatile value_type &>(v_) = v;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "lock ; xaddw %0, %1"
            : "+q" (v), "+m" (v_)
            :
            : "cc"
        );
        platform_fence_after(order);
        return v;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fetch_add(-v, order);
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgw %0, %1"
            : "+q" (v), "+m" (v_)
        );
        platform_fence_after(order);
        return v;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        value_type previous = expected;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgw %3, %1\n\t"
            "sete %2"
            : "+a" (previous), "+m" (v_), "=q" (success)
            : "q" (desired)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        expected = previous;
        return success;
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

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp & v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp | v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp ^ v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
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
        if (order != memory_order_seq_cst) {
            platform_fence_before(order);
            const_cast<volatile value_type &>(v_) = v;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "lock ; xaddl %0, %1"
            : "+r" (v), "+m" (v_)
            :
            : "cc"
        );
        platform_fence_after(order);
        return v;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fetch_add(-v, order);
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgl %0, %1"
            : "+r" (v), "+m" (v_)
        );
        platform_fence_after(order);
        return v;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        value_type previous = expected;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgl %3, %1\n\t"
            "sete %2"
            : "+a,a" (previous), "+m,m" (v_), "=q,m" (success)
            : "r,r" (desired)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        expected = previous;
        return success;
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

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp & v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp | v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp ^ v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
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

#if defined(__x86_64__)
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
        if (order != memory_order_seq_cst) {
            platform_fence_before(order);
            const_cast<volatile value_type &>(v_) = v;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type
    fetch_add(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "lock ; xaddq %0, %1"
            : "+r" (v), "+m" (v_)
            :
            : "cc"
        );
        platform_fence_after(order);
        return v;
    }

    value_type
    fetch_sub(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fetch_add(-v, order);
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgq %0, %1"
            : "+r" (v), "+m" (v_)
        );
        platform_fence_after(order);
        return v;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        value_type previous = expected;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgq %3, %1\n\t"
            "sete %2"
            : "+a,a" (previous), "+m,m" (v_), "=q,m" (success)
            : "r,r" (desired)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        expected = previous;
        return success;
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

    value_type
    fetch_and(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp & v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
    }

    value_type
    fetch_or(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp | v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
    }

    value_type
    fetch_xor(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        value_type tmp = load(memory_order_relaxed);
        while (!compare_exchange_weak(tmp, tmp ^ v, order, memory_order_relaxed))
        {
            BOOST_ATOMIC_X86_PAUSE();
        }
        return tmp;
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

/* pointers */

// NOTE: x32 target is still regarded to as x86_64 and can only be detected by the size of pointers
#if !defined(__x86_64__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4)

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
        if (order != memory_order_seq_cst) {
            platform_fence_before(order);
            const_cast<volatile value_type &>(v_) = v;
        } else {
            exchange(v, order);
        }
    }

    value_type load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgl %0, %1"
            : "+r" (v), "+m" (v_)
        );
        platform_fence_after(order);
        return v;
    }

    bool compare_exchange_strong(value_type & expected, value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        value_type previous = expected;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgl %3, %1\n\t"
            "sete %2"
            : "+a,a" (previous), "+m,m" (v_), "=q,m" (success)
            : "r,r" (desired)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        expected = previous;
        return success;
    }

    bool compare_exchange_weak(value_type & expected, value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, success_order, failure_order);
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    value_type
    fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "lock ; xaddl %0, %1"
            : "+r" (v), "+m" (v_)
            :
            : "cc"
        );
        platform_fence_after(order);
        return reinterpret_cast<value_type>(v);
    }

    value_type
    fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fetch_add(-v, order);
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
        if (order != memory_order_seq_cst) {
            platform_fence_before(order);
            const_cast<volatile value_type &>(v_) = v;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgl %0, %1"
            : "+r" (v), "+m" (v_)
        );
        platform_fence_after(order);
        return v;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        value_type previous = expected;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgl %3, %1\n\t"
            "sete %2"
            : "+a,a" (previous), "+m,m" (v_), "=q,m" (success)
            : "r,r" (desired)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        expected = previous;
        return success;
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

    value_type
    fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        v = v * sizeof(*v_);
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "lock ; xaddl %0, %1"
            : "+r" (v), "+m" (v_)
            :
            : "cc"
        );
        platform_fence_after(order);
        return reinterpret_cast<value_type>(v);
    }

    value_type
    fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fetch_add(-v, order);
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
        if (order != memory_order_seq_cst) {
            platform_fence_before(order);
            const_cast<volatile value_type &>(v_) = v;
        } else {
            exchange(v, order);
        }
    }

    value_type load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgq %0, %1"
            : "+r" (v), "+m" (v_)
        );
        platform_fence_after(order);
        return v;
    }

    bool compare_exchange_strong(value_type & expected, value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        value_type previous = expected;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgq %3, %1\n\t"
            "sete %2"
            : "+a,a" (previous), "+m,m" (v_), "=q,m" (success)
            : "r,r" (desired)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        expected = previous;
        return success;
    }

    bool compare_exchange_weak(value_type & expected, value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, success_order, failure_order);
    }

    bool
    is_lock_free(void) const volatile BOOST_NOEXCEPT
    {
        return true;
    }

    value_type
    fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "lock ; xaddq %0, %1"
            : "+r" (v), "+m" (v_)
            :
            : "cc"
        );
        platform_fence_after(order);
        return reinterpret_cast<value_type>(v);
    }

    value_type
    fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fetch_add(-v, order);
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
        if (order != memory_order_seq_cst) {
            platform_fence_before(order);
            const_cast<volatile value_type &>(v_) = v;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        value_type v = const_cast<const volatile value_type &>(v_);
        platform_fence_after_load(order);
        return v;
    }

    value_type
    exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgq %0, %1"
            : "+r" (v), "+m" (v_)
        );
        platform_fence_after(order);
        return v;
    }

    bool
    compare_exchange_strong(
        value_type & expected,
        value_type desired,
        memory_order success_order,
        memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        value_type previous = expected;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgq %3, %1\n\t"
            "sete %2"
            : "+a,a" (previous), "+m,m" (v_), "=q,m" (success)
            : "r,r" (desired)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        expected = previous;
        return success;
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

    value_type
    fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        v = v * sizeof(*v_);
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "lock ; xaddq %0, %1"
            : "+r" (v), "+m" (v_)
            :
            : "cc"
        );
        platform_fence_after(order);
        return reinterpret_cast<value_type>(v);
    }

    value_type
    fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fetch_add(-v, order);
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

template<typename T, bool Sign>
class base_atomic<T, void, 1, Sign>
{
private:
    typedef base_atomic this_type;
    typedef T value_type;
    typedef uint8_t storage_type;

protected:
    typedef value_type const& value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type const& v) BOOST_NOEXCEPT :
        v_(reinterpret_cast<storage_type const&>(v))
    {
    }

    void
    store(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        if (order != memory_order_seq_cst) {
            storage_type tmp;
            memcpy(&tmp, &v, sizeof(value_type));
            platform_fence_before(order);
            const_cast<volatile storage_type &>(v_) = tmp;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type tmp = const_cast<volatile storage_type &>(v_);
        platform_fence_after_load(order);
        value_type v;
        memcpy(&v, &tmp, sizeof(value_type));
        return v;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp;
        memcpy(&tmp, &v, sizeof(value_type));
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgb %0, %1"
            : "+q" (tmp), "+m" (v_)
        );
        platform_fence_after(order);
        value_type res;
        memcpy(&res, &tmp, sizeof(value_type));
        return res;
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
        storage_type previous_s = expected_s;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgb %3, %1\n\t"
            "sete %2"
            : "+a" (previous_s), "+m" (v_), "=q" (success)
            : "q" (desired_s)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        memcpy(&expected, &previous_s, sizeof(value_type));
        return success;
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
    typedef uint16_t storage_type;

protected:
    typedef value_type const& value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic(void), {})
    BOOST_CONSTEXPR explicit base_atomic(value_type const& v) BOOST_NOEXCEPT :
        v_(reinterpret_cast<storage_type const&>(v))
    {
    }

    void
    store(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        if (order != memory_order_seq_cst) {
            storage_type tmp;
            memcpy(&tmp, &v, sizeof(value_type));
            platform_fence_before(order);
            const_cast<volatile storage_type &>(v_) = tmp;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type tmp = const_cast<volatile storage_type &>(v_);
        platform_fence_after_load(order);
        value_type v;
        memcpy(&v, &tmp, sizeof(value_type));
        return v;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp;
        memcpy(&tmp, &v, sizeof(value_type));
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgw %0, %1"
            : "+q" (tmp), "+m" (v_)
        );
        platform_fence_after(order);
        value_type res;
        memcpy(&res, &tmp, sizeof(value_type));
        return res;
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
        storage_type previous_s = expected_s;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgw %3, %1\n\t"
            "sete %2"
            : "+a" (previous_s), "+m" (v_), "=q" (success)
            : "q" (desired_s)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        memcpy(&expected, &previous_s, sizeof(value_type));
        return success;
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
        if (order != memory_order_seq_cst) {
            storage_type tmp = 0;
            memcpy(&tmp, &v, sizeof(value_type));
            platform_fence_before(order);
            const_cast<volatile storage_type &>(v_) = tmp;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type tmp = const_cast<volatile storage_type &>(v_);
        platform_fence_after_load(order);
        value_type v;
        memcpy(&v, &tmp, sizeof(value_type));
        return v;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0;
        memcpy(&tmp, &v, sizeof(value_type));
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgl %0, %1"
            : "+q" (tmp), "+m" (v_)
        );
        platform_fence_after(order);
        value_type res;
        memcpy(&res, &tmp, sizeof(value_type));
        return res;
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
        storage_type previous_s = expected_s;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgl %3, %1\n\t"
            "sete %2"
            : "+a,a" (previous_s), "+m,m" (v_), "=q,m" (success)
            : "q,q" (desired_s)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        memcpy(&expected, &previous_s, sizeof(value_type));
        return success;
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

#if defined(__x86_64__)
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
        if (order != memory_order_seq_cst) {
            storage_type tmp = 0;
            memcpy(&tmp, &v, sizeof(value_type));
            platform_fence_before(order);
            const_cast<volatile storage_type &>(v_) = tmp;
        } else {
            exchange(v, order);
        }
    }

    value_type
    load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        storage_type tmp = const_cast<volatile storage_type &>(v_);
        platform_fence_after_load(order);
        value_type v;
        memcpy(&v, &tmp, sizeof(value_type));
        return v;
    }

    value_type
    exchange(value_type const& v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        storage_type tmp = 0;
        memcpy(&tmp, &v, sizeof(value_type));
        platform_fence_before(order);
        __asm__ __volatile__
        (
            "xchgq %0, %1"
            : "+q" (tmp), "+m" (v_)
        );
        platform_fence_after(order);
        value_type res;
        memcpy(&res, &tmp, sizeof(value_type));
        return res;
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
        storage_type previous_s = expected_s;
        platform_fence_before(success_order);
        bool success;
        __asm__ __volatile__
        (
            "lock ; cmpxchgq %3, %1\n\t"
            "sete %2"
            : "+a,a" (previous_s), "+m,m" (v_), "=q,m" (success)
            : "q,q" (desired_s)
            : "cc"
        );
        if (success)
            platform_fence_after(success_order);
        else
            platform_fence_after(failure_order);
        memcpy(&expected, &previous_s, sizeof(value_type));
        return success;
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

#if !defined(__x86_64__) && defined(BOOST_ATOMIC_X86_HAS_CMPXCHG8B)

template<typename T>
inline bool
platform_cmpxchg64_strong(T & expected, T desired, volatile T * ptr) BOOST_NOEXCEPT
{
#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8
    const T oldval = __sync_val_compare_and_swap(ptr, expected, desired);
    const bool result = (oldval == expected);
    expected = oldval;
    return result;
#else
    uint32_t scratch;
    /* Make sure ebx is saved and restored properly in case
    this object is compiled as "position independent". Since
    programmers on x86 tend to forget specifying -DPIC or
    similar, always assume PIC.

    To make this work uniformly even in the non-PIC case,
    setup register constraints such that ebx can not be
    used by accident e.g. as base address for the variable
    to be modified. Accessing "scratch" should always be okay,
    as it can only be placed on the stack (and therefore
    accessed through ebp or esp only).

    In theory, could push/pop ebx onto/off the stack, but movs
    to a prepared stack slot turn out to be faster. */
    bool success;
    __asm__ __volatile__
    (
        "movl %%ebx, %[scratch]\n\t"
        "movl %[desired_lo], %%ebx\n\t"
        "lock; cmpxchg8b %[dest]\n\t"
        "movl %[scratch], %%ebx\n\t"
        "sete %[success]"
        : "+A,A,A,A,A,A" (expected), [dest] "+m,m,m,m,m,m" (*ptr), [scratch] "=m,m,m,m,m,m" (scratch), [success] "=q,m,q,m,q,m" (success)
        : [desired_lo] "S,S,D,D,m,m" ((uint32_t)desired), "c,c,c,c,c,c" ((uint32_t)(desired >> 32))
        : "memory", "cc"
    );
    return success;
#endif
}

// Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A, 8.1.1. Guaranteed Atomic Operations:
//
// The Pentium processor (and newer processors since) guarantees that the following additional memory operations will always be carried out atomically:
// * Reading or writing a quadword aligned on a 64-bit boundary
//
// Luckily, the memory is almost always 8-byte aligned in our case because atomic<> uses 64 bit native types for storage and dynamic memory allocations
// have at least 8 byte alignment. The only unfortunate case is when atomic is placeod on the stack and it is not 8-byte aligned (like on 32 bit Windows).

template<typename T>
inline void
platform_store64(T value, volatile T * ptr) BOOST_NOEXCEPT
{
    if (((uint32_t)ptr & 0x00000007) == 0)
    {
#if defined(__SSE2__)
        __asm__ __volatile__
        (
            "movq %1, %%xmm4\n\t"
            "movq %%xmm4, %0\n\t"
            : "=m" (*ptr)
            : "m" (value)
            : "memory", "xmm4"
        );
#else
        __asm__ __volatile__
        (
            "fildll %1\n\t"
            "fistpll %0\n\t"
            : "=m" (*ptr)
            : "m" (value)
            : "memory"
        );
#endif
    }
    else
    {
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
            : [value_lo] "a,a" ((uint32_t)value), "c,c" ((uint32_t)(value >> 32)), [dest] "D,S" (ptr)
            : "memory", "cc", "edx"
        );
    }
}

template<typename T>
inline T
platform_load64(const volatile T * ptr) BOOST_NOEXCEPT
{
    T value;

    if (((uint32_t)ptr & 0x00000007) == 0)
    {
#if defined(__SSE2__)
        __asm__ __volatile__
        (
            "movq %1, %%xmm4\n\t"
            "movq %%xmm4, %0\n\t"
            : "=m" (value)
            : "m" (*ptr)
            : "memory", "xmm4"
        );
#else
        __asm__ __volatile__
        (
            "fildll %1\n\t"
            "fistpll %0\n\t"
            : "=m" (value)
            : "m" (*ptr)
            : "memory"
        );
#endif
    }
    else
    {
        // We don't care for comparison result here; the previous value will be stored into value anyway.
        // Also we don't care for ebx and ecx values, they just have to be equal to eax and edx before cmpxchg8b.
        __asm__ __volatile__
        (
            "movl %%ebx, %%eax\n\t"
            "movl %%ecx, %%edx\n\t"
            "lock; cmpxchg8b %[dest]"
            : "=&A" (value)
            : [dest] "m" (*ptr)
            : "cc"
        );
    }

    return value;
}

#endif

#if defined(BOOST_ATOMIC_INT128_LOCK_FREE) && BOOST_ATOMIC_INT128_LOCK_FREE > 0

template<typename T>
inline bool
platform_cmpxchg128_strong(T& expected, T desired, volatile T* ptr) BOOST_NOEXCEPT
{
    uint64_t const* p_desired = (uint64_t const*)&desired;
    bool success;
    __asm__ __volatile__
    (
        "lock; cmpxchg16b %[dest]\n\t"
        "sete %[success]"
        : "+A,A" (expected), [dest] "+m,m" (*ptr), [success] "=q,m" (success)
        : "b,b" (p_desired[0]), "c,c" (p_desired[1])
        : "memory", "cc"
    );
    return success;
}

template<typename T>
inline void
platform_store128(T value, volatile T* ptr) BOOST_NOEXCEPT
{
    uint64_t const* p_value = (uint64_t const*)&value;
    __asm__ __volatile__
    (
        "movq 0(%[dest]), %%rax\n\t"
        "movq 8(%[dest]), %%rdx\n\t"
        ".align 16\n\t"
        "1: lock; cmpxchg16b 0(%[dest])\n\t"
        "jne 1b"
        :
        : "b" (p_value[0]), "c" (p_value[1]), [dest] "r" (ptr)
        : "memory", "cc", "rax", "rdx"
    );
}

template<typename T>
inline T
platform_load128(const volatile T* ptr) BOOST_NOEXCEPT
{
    T value;

    // We don't care for comparison result here; the previous value will be stored into value anyway.
    // Also we don't care for rbx and rcx values, they just have to be equal to rax and rdx before cmpxchg16b.
    __asm__ __volatile__
    (
        "movq %%rbx, %%rax\n\t"
        "movq %%rcx, %%rdx\n\t"
        "lock; cmpxchg16b %[dest]"
        : "=&A" (value)
        : [dest] "m" (*ptr)
        : "cc"
    );

    return value;
}

#endif // defined(BOOST_ATOMIC_INT128_LOCK_FREE) && BOOST_ATOMIC_INT128_LOCK_FREE > 0

}
}
}

/* pull in 64-bit atomic type using cmpxchg8b above */
#if !defined(__x86_64__) && defined(BOOST_ATOMIC_X86_HAS_CMPXCHG8B)
#include <boost/atomic/detail/cas64strong.hpp>
#endif

/* pull in 128-bit atomic type using cmpxchg16b above */
#if defined(BOOST_ATOMIC_INT128_LOCK_FREE) && BOOST_ATOMIC_INT128_LOCK_FREE > 0
#include <boost/atomic/detail/cas128strong.hpp>
#endif

#endif /* !defined(BOOST_ATOMIC_FORCE_FALLBACK) */

#endif
