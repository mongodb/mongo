// -----------------------------------------------------------
// integer_log2.hpp
//
//   Gives the integer part of the logarithm, in base 2, of a
// given number. Behavior is undefined if the argument is <= 0.
//
//        Copyright (c) 2003-2004, 2008 Gennaro Prota
//            Copyright (c) 2022 Andrey Semashev
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          https://www.boost.org/LICENSE_1_0.txt)
//
// -----------------------------------------------------------

#ifndef BOOST_INTEGER_INTEGER_LOG2_HPP
#define BOOST_INTEGER_INTEGER_LOG2_HPP

#include <climits>
#include <limits>
#include <boost/config.hpp>
#include <boost/assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/core/bit.hpp>
#include <boost/core/enable_if.hpp>
#include <boost/type_traits/is_integral.hpp>
#include <boost/type_traits/make_unsigned.hpp>

namespace boost {
namespace detail {

// helper to find the maximum power of two
// less than p
template< unsigned int p, unsigned int n, bool = ((2u * n) < p) >
struct max_pow2_less :
    public max_pow2_less< p, 2u * n >
{
};

template< unsigned int p, unsigned int n >
struct max_pow2_less< p, n, false >
{
    BOOST_STATIC_CONSTANT(unsigned int, value = n);
};

template< typename T >
inline typename boost::disable_if< boost::is_integral< T >, int >::type integer_log2_impl(T x)
{
    unsigned int n = detail::max_pow2_less<
        std::numeric_limits< T >::digits,
        CHAR_BIT / 2u
    >::value;

    int result = 0;
    while (x != 1)
    {
        T t(x >> n);
        if (t)
        {
            result += static_cast< int >(n);
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
            x = static_cast< T&& >(t);
#else
            x = t;
#endif
        }
        n >>= 1u;
    }

    return result;
}

template< typename T >
inline typename boost::enable_if< boost::is_integral< T >, int >::type integer_log2_impl(T x)
{
    // We could simply rely on numeric_limits but sometimes
    // Borland tries to use numeric_limits<const T>, because
    // of its usual const-related problems in argument deduction
    // - gps
    return static_cast< int >((sizeof(T) * CHAR_BIT - 1u) -
        boost::core::countl_zero(static_cast< typename boost::make_unsigned< T >::type >(x)));
}

#if defined(BOOST_HAS_INT128)
// We need to provide explicit overloads for __int128 because (a) boost/core/bit.hpp currently does not support it and
// (b) std::numeric_limits are not specialized for __int128 in some standard libraries.
inline int integer_log2_impl(boost::uint128_type x)
{
    const boost::uint64_t x_hi = static_cast< boost::uint64_t >(x >> 64u);
    if (x_hi != 0u)
        return 127 - boost::core::countl_zero(x_hi);
    else
        return 63 - boost::core::countl_zero(static_cast< boost::uint64_t >(x));
}

inline int integer_log2_impl(boost::int128_type x)
{
    return detail::integer_log2_impl(static_cast< boost::uint128_type >(x));
}
#endif // defined(BOOST_HAS_INT128)

} // namespace detail


// ------------
// integer_log2
// ------------
template< typename T >
inline int integer_log2(T x)
{
    BOOST_ASSERT(x > 0);
    return detail::integer_log2_impl(x);
}

} // namespace boost

#endif // BOOST_INTEGER_INTEGER_LOG2_HPP
