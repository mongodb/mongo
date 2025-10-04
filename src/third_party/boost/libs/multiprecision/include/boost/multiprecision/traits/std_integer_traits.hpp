///////////////////////////////////////////////////////////////
//  Copyright 2012-2022 John Maddock.
//  Copyright 2022 Matt Borland. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_STD_INTEGER_TRAITS_HPP
#define BOOST_MP_STD_INTEGER_TRAITS_HPP

#include <type_traits>
#include <boost/multiprecision/detail/standalone_config.hpp>

namespace boost {
namespace multiprecision {
namespace detail {

template <class T>
struct is_signed : public std::is_signed<T> {};
template <class T>
struct is_unsigned : public std::is_unsigned<T> {};
template <class T>
struct is_integral : public std::is_integral<T> {};
template <class T>
struct is_arithmetic : public std::is_arithmetic<T> {};
template <class T>
struct make_unsigned : public std::make_unsigned<T> {};
template <class T>
struct make_signed : public std::make_signed<T> {};

#if defined(__SIZEOF_FLOAT128__)
template <>
struct is_arithmetic<__float128> : public std::true_type {};
#endif

#ifdef BOOST_HAS_INT128

template <>
struct is_signed<int128_type> : public std::true_type {};
template <>
struct is_signed<uint128_type> : public std::false_type {};
template <>
struct is_unsigned<int128_type> : public std::false_type {};
template <>
struct is_unsigned<uint128_type> : public std::true_type {};
template <>
struct is_integral<int128_type> : public std::true_type {};
template <>
struct is_integral<uint128_type> : public std::true_type {};
template <>
struct is_arithmetic<int128_type> : public std::true_type {};
template <>
struct is_arithmetic<uint128_type> : public std::true_type {};
template <>
struct make_unsigned<int128_type>
{
   using type = uint128_type;
};
template <>
struct make_unsigned<uint128_type>
{
   using type = uint128_type;
};
template <>
struct make_signed<int128_type>
{
   using type = int128_type;
};
template <>
struct make_signed<uint128_type>
{
   using type = int128_type;
};

#endif

// C++17-esque helpers
#if defined(__cpp_variable_templates) && __cpp_variable_templates >= 201304L
template <typename T>
BOOST_INLINE_CONSTEXPR bool is_signed_v = is_signed<T>::value;
template <typename T>
BOOST_INLINE_CONSTEXPR bool is_unsigned_v = is_unsigned<T>::value;
template <typename T>
BOOST_INLINE_CONSTEXPR bool is_integral_v = is_integral<T>::value;
template <typename T>
BOOST_INLINE_CONSTEXPR bool is_arithmetic_v = is_arithmetic<T>::value;
#endif

template <typename T>
using make_unsigned_t = typename make_unsigned<T>::type;
template <typename T>
using make_signed_t = typename make_signed<T>::type;

}}} // namespace boost::multiprecision::detail

#endif
