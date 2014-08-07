#ifndef BOOST_ATOMIC_ATOMIC_HPP
#define BOOST_ATOMIC_ATOMIC_HPP

//  Copyright (c) 2011 Helge Bahmann
//  Copyright (c) 2013 Tim Blechmann
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <cstddef>
#include <boost/cstdint.hpp>

#include <boost/memory_order.hpp>

#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/platform.hpp>
#include <boost/atomic/detail/type-classification.hpp>
#include <boost/type_traits/is_signed.hpp>
#if defined(BOOST_MSVC) && BOOST_MSVC < 1400
#include <boost/type_traits/is_integral.hpp>
#include <boost/mpl/and.hpp>
#endif

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {

#ifndef BOOST_ATOMIC_CHAR_LOCK_FREE
#define BOOST_ATOMIC_CHAR_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_CHAR16_T_LOCK_FREE
#define BOOST_ATOMIC_CHAR16_T_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_CHAR32_T_LOCK_FREE
#define BOOST_ATOMIC_CHAR32_T_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_WCHAR_T_LOCK_FREE
#define BOOST_ATOMIC_WCHAR_T_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_SHORT_LOCK_FREE
#define BOOST_ATOMIC_SHORT_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_INT_LOCK_FREE
#define BOOST_ATOMIC_INT_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_LONG_LOCK_FREE
#define BOOST_ATOMIC_LONG_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_LLONG_LOCK_FREE
#define BOOST_ATOMIC_LLONG_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_INT128_LOCK_FREE
#define BOOST_ATOMIC_INT128_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_POINTER_LOCK_FREE
#define BOOST_ATOMIC_POINTER_LOCK_FREE 0
#endif

#define BOOST_ATOMIC_ADDRESS_LOCK_FREE BOOST_ATOMIC_POINTER_LOCK_FREE

#ifndef BOOST_ATOMIC_BOOL_LOCK_FREE
#define BOOST_ATOMIC_BOOL_LOCK_FREE 0
#endif

#ifndef BOOST_ATOMIC_THREAD_FENCE
#define BOOST_ATOMIC_THREAD_FENCE 0
inline void atomic_thread_fence(memory_order)
{
}
#endif

#ifndef BOOST_ATOMIC_SIGNAL_FENCE
#define BOOST_ATOMIC_SIGNAL_FENCE 0
inline void atomic_signal_fence(memory_order order)
{
    atomic_thread_fence(order);
}
#endif

template<typename T>
class atomic :
    public atomics::detail::base_atomic<
        T,
        typename atomics::detail::classify<T>::type,
        atomics::detail::storage_size_of<T>::value,
#if !defined(BOOST_MSVC) || BOOST_MSVC >= 1400
        boost::is_signed<T>::value
#else
        // MSVC 2003 has problems instantiating is_signed on non-integral types
        mpl::and_< boost::is_integral<T>, boost::is_signed<T> >::value
#endif
    >
{
private:
    typedef T value_type;
    typedef atomics::detail::base_atomic<
        T,
        typename atomics::detail::classify<T>::type,
        atomics::detail::storage_size_of<T>::value,
#if !defined(BOOST_MSVC) || BOOST_MSVC >= 1400
        boost::is_signed<T>::value
#else
        // MSVC 2003 has problems instantiating is_signed on non-itegral types
        mpl::and_< boost::is_integral<T>, boost::is_signed<T> >::value
#endif
    > super;
    typedef typename super::value_arg_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(atomic(void), BOOST_NOEXCEPT {})

    // NOTE: The constructor is made explicit because gcc 4.7 complains that
    //       operator=(value_arg_type) is considered ambiguous with operator=(atomic const&)
    //       in assignment expressions, even though conversion to atomic<> is less preferred
    //       than conversion to value_arg_type.
    explicit BOOST_CONSTEXPR atomic(value_arg_type v) BOOST_NOEXCEPT : super(v) {}

    value_type operator=(value_arg_type v) volatile BOOST_NOEXCEPT
    {
        this->store(v);
        return v;
    }

    operator value_type(void) volatile const BOOST_NOEXCEPT
    {
        return this->load();
    }

    BOOST_DELETED_FUNCTION(atomic(atomic const&))
    BOOST_DELETED_FUNCTION(atomic& operator=(atomic const&) volatile)
};

typedef atomic<char> atomic_char;
typedef atomic<unsigned char> atomic_uchar;
typedef atomic<signed char> atomic_schar;
typedef atomic<uint8_t> atomic_uint8_t;
typedef atomic<int8_t> atomic_int8_t;
typedef atomic<unsigned short> atomic_ushort;
typedef atomic<short> atomic_short;
typedef atomic<uint16_t> atomic_uint16_t;
typedef atomic<int16_t> atomic_int16_t;
typedef atomic<unsigned int> atomic_uint;
typedef atomic<int> atomic_int;
typedef atomic<uint32_t> atomic_uint32_t;
typedef atomic<int32_t> atomic_int32_t;
typedef atomic<unsigned long> atomic_ulong;
typedef atomic<long> atomic_long;
typedef atomic<uint64_t> atomic_uint64_t;
typedef atomic<int64_t> atomic_int64_t;
#ifdef BOOST_HAS_LONG_LONG
typedef atomic<boost::ulong_long_type> atomic_ullong;
typedef atomic<boost::long_long_type> atomic_llong;
#endif
typedef atomic<void*> atomic_address;
typedef atomic<bool> atomic_bool;
typedef atomic<wchar_t> atomic_wchar_t;
#if !defined(BOOST_NO_CXX11_CHAR16_T)
typedef atomic<char16_t> atomic_char16_t;
#endif
#if !defined(BOOST_NO_CXX11_CHAR32_T)
typedef atomic<char32_t> atomic_char32_t;
#endif

typedef atomic<int_least8_t> atomic_int_least8_t;
typedef atomic<uint_least8_t> atomic_uint_least8_t;
typedef atomic<int_least16_t> atomic_int_least16_t;
typedef atomic<uint_least16_t> atomic_uint_least16_t;
typedef atomic<int_least32_t> atomic_int_least32_t;
typedef atomic<uint_least32_t> atomic_uint_least32_t;
typedef atomic<int_least64_t> atomic_int_least64_t;
typedef atomic<uint_least64_t> atomic_uint_least64_t;
typedef atomic<int_fast8_t> atomic_int_fast8_t;
typedef atomic<uint_fast8_t> atomic_uint_fast8_t;
typedef atomic<int_fast16_t> atomic_int_fast16_t;
typedef atomic<uint_fast16_t> atomic_uint_fast16_t;
typedef atomic<int_fast32_t> atomic_int_fast32_t;
typedef atomic<uint_fast32_t> atomic_uint_fast32_t;
typedef atomic<int_fast64_t> atomic_int_fast64_t;
typedef atomic<uint_fast64_t> atomic_uint_fast64_t;
typedef atomic<intmax_t> atomic_intmax_t;
typedef atomic<uintmax_t> atomic_uintmax_t;

typedef atomic<std::size_t> atomic_size_t;
typedef atomic<std::ptrdiff_t> atomic_ptrdiff_t;

#if defined(BOOST_HAS_INTPTR_T)
typedef atomic<intptr_t> atomic_intptr_t;
typedef atomic<uintptr_t> atomic_uintptr_t;
#endif

#ifndef BOOST_ATOMIC_FLAG_LOCK_FREE
#define BOOST_ATOMIC_FLAG_LOCK_FREE 0
class atomic_flag
{
public:
    BOOST_CONSTEXPR atomic_flag(void) BOOST_NOEXCEPT : v_(false) {}

    bool
    test_and_set(memory_order order = memory_order_seq_cst) BOOST_NOEXCEPT
    {
        return v_.exchange(true, order);
    }

    void
    clear(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        v_.store(false, order);
    }

    BOOST_DELETED_FUNCTION(atomic_flag(atomic_flag const&))
    BOOST_DELETED_FUNCTION(atomic_flag& operator=(atomic_flag const&))

private:
    atomic<bool> v_;
};
#endif

}

#endif
