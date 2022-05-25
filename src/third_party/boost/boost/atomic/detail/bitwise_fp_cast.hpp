/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2018, 2021 Andrey Semashev
 */
/*!
 * \file   atomic/detail/bitwise_fp_cast.hpp
 *
 * This header defines \c bitwise_fp_cast used to convert between storage and floating point value types
 */

#ifndef BOOST_ATOMIC_DETAIL_BITWISE_FP_CAST_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_BITWISE_FP_CAST_HPP_INCLUDED_

#include <cstddef>
#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/float_sizes.hpp>
#include <boost/atomic/detail/bitwise_cast.hpp>
#if defined(BOOST_ATOMIC_DETAIL_BIT_CAST)
#include <boost/atomic/detail/type_traits/integral_constant.hpp>
#endif
#include <boost/atomic/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace atomics {
namespace detail {

/*!
 * \brief The type trait returns the size of the value of the specified floating point type
 *
 * This size may be less than <tt>sizeof(T)</tt> if the implementation uses padding bytes for a particular FP type. This is
 * often the case with 80-bit extended double, which is stored in 12 or 16 initial bytes with tail padding filled with garbage.
 */
template< typename T >
struct value_size_of
{
    static BOOST_CONSTEXPR_OR_CONST std::size_t value = sizeof(T);
};

#if defined(BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE)
template< >
struct value_size_of< float >
{
    static BOOST_CONSTEXPR_OR_CONST std::size_t value = BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE;
};
#endif

#if defined(BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE)
template< >
struct value_size_of< double >
{
    static BOOST_CONSTEXPR_OR_CONST std::size_t value = BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE;
};
#endif

#if defined(BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE)
template< >
struct value_size_of< long double >
{
    static BOOST_CONSTEXPR_OR_CONST std::size_t value = BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE;
};
#endif

template< typename T >
struct value_size_of< const T > : value_size_of< T > {};

template< typename T >
struct value_size_of< volatile T > : value_size_of< T > {};

template< typename T >
struct value_size_of< const volatile T > : value_size_of< T > {};


#if !defined(BOOST_ATOMIC_NO_CLEAR_PADDING)
// BOOST_ATOMIC_DETAIL_CLEAR_PADDING, which is used in bitwise_cast, will clear the tail padding bits in the source object.
// We don't need to specify the actual value size to avoid redundant zeroing of the tail padding.
#define BOOST_ATOMIC_DETAIL_BITWISE_FP_CAST_VALUE_SIZE_OF(x) sizeof(x)
#else
#define BOOST_ATOMIC_DETAIL_BITWISE_FP_CAST_VALUE_SIZE_OF(x) atomics::detail::value_size_of< x >::value
#endif

#if defined(BOOST_ATOMIC_DETAIL_BIT_CAST)

//! Similar to bitwise_cast, but either \c From or \c To is expected to be a floating point type. Attempts to detect the actual value size in the source object and considers the rest of the object as padding.
template< typename To, typename From >
BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_BITWISE_CAST To bitwise_fp_cast(From const& from) BOOST_NOEXCEPT
{
    // For floating point types, has_unique_object_representations is typically false even if the type contains no padding bits.
    // Here, we rely on our detection of the actual value size to select constexpr bit_cast implementation when possible. We assume
    // here that floating point value bits are contiguous.
    return atomics::detail::bitwise_cast_impl< To, BOOST_ATOMIC_DETAIL_BITWISE_FP_CAST_VALUE_SIZE_OF(From) >(from, atomics::detail::integral_constant< bool,
        atomics::detail::value_size_of< From >::value == sizeof(From) && atomics::detail::value_size_of< From >::value == sizeof(To) >());
}

#else // defined(BOOST_ATOMIC_DETAIL_BIT_CAST)

//! Similar to bitwise_cast, but either \c From or \c To is expected to be a floating point type. Attempts to detect the actual value size in the source object and considers the rest of the object as padding.
template< typename To, typename From >
BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_BITWISE_CAST To bitwise_fp_cast(From const& from) BOOST_NOEXCEPT
{
    return atomics::detail::bitwise_cast< To, BOOST_ATOMIC_DETAIL_BITWISE_FP_CAST_VALUE_SIZE_OF(From) >(from);
}

#endif // defined(BOOST_ATOMIC_DETAIL_BIT_CAST)

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>

#endif // BOOST_ATOMIC_DETAIL_BITWISE_FP_CAST_HPP_INCLUDED_
