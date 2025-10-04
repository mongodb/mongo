
//  Copyright 2012 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_CPP_INT_CHECKED_HPP
#define BOOST_MP_CPP_INT_CHECKED_HPP

#include <climits>
#include <limits>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <boost/multiprecision/detail/standalone_config.hpp>
#include <boost/multiprecision/detail/no_exceptions_support.hpp>

namespace boost { namespace multiprecision { namespace backends { namespace detail {

//
// Simple routines for performing checked arithmetic with a builtin arithmetic type.
// Note that this is not a complete header, it must be included as part of boost/multiprecision/cpp_int.hpp.
//

template <typename T>
inline constexpr T type_max() noexcept
{
   return 
   #ifdef BOOST_HAS_INT128
   std::is_same<T, boost::multiprecision::int128_type>::value ? INT128_MAX :
   std::is_same<T, boost::multiprecision::uint128_type>::value ? UINT128_MAX :
   #endif
   (std::numeric_limits<T>::max)();
}

template <typename T>
inline constexpr T type_min() noexcept
{
   return 
   #ifdef BOOST_HAS_INT128
   std::is_same<T, boost::multiprecision::int128_type>::value ? INT128_MIN :
   std::is_same<T, boost::multiprecision::uint128_type>::value ? T(0) :
   #endif
   (std::numeric_limits<T>::min)();
}

inline void raise_overflow(std::string op)
{
   BOOST_MP_THROW_EXCEPTION(std::overflow_error("overflow in " + op));
}
inline void raise_add_overflow()
{
   raise_overflow("addition");
}
inline void raise_subtract_overflow()
{
   BOOST_MP_THROW_EXCEPTION(std::range_error("Subtraction resulted in a negative value, but the type is unsigned"));
}
inline void raise_mul_overflow()
{
   raise_overflow("multiplication");
}
inline void raise_div_overflow()
{
   raise_overflow("division");
}

template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_add_imp(A a, A b, const std::integral_constant<bool, true>&)
{
   if (a > 0)
   {
      if ((b > 0) && ((type_max<A>() - b) < a))
         raise_add_overflow();
   }
   else
   {
      if ((b < 0) && ((type_min<A>() - b) > a))
         raise_add_overflow();
   }
   return a + b;
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_add_imp(A a, A b, const std::integral_constant<bool, false>&)
{
   if ((type_max<A>() - b) < a)
      raise_add_overflow();
   return a + b;
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_add(A a, A b, const std::integral_constant<int, checked>&)
{
   return checked_add_imp(a, b, std::integral_constant<bool, boost::multiprecision::detail::is_signed<A>::value && boost::multiprecision::detail::is_integral<A>::value > ());
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_add(A a, A b, const std::integral_constant<int, unchecked>&)
{
   return a + b;
}

template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_subtract_imp(A a, A b, const std::integral_constant<bool, true>&)
{
   if (a > 0)
   {
      if ((b < 0) && ((type_max<A>() + b) < a))
         raise_subtract_overflow();
   }
   else
   {
      if ((b > 0) && ((type_min<A>() + b) > a))
         raise_subtract_overflow();
   }
   return a - b;
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_subtract_imp(A a, A b, const std::integral_constant<bool, false>&)
{
   if (a < b)
      raise_subtract_overflow();
   return a - b;
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_subtract(A a, A b, const std::integral_constant<int, checked>&)
{
   return checked_subtract_imp(a, b, std::integral_constant<bool, boost::multiprecision::detail::is_signed<A>::value && boost::multiprecision::detail::is_integral<A>::value>());
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_subtract(A a, A b, const std::integral_constant<int, unchecked>&)
{
   return a - b;
}

template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_multiply(A a, A b, const std::integral_constant<int, checked>&)
{
   BOOST_MP_USING_ABS
   if (a && (type_max<A>() / abs(a) < abs(b)))
      raise_mul_overflow();
   return a * b;
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_multiply(A a, A b, const std::integral_constant<int, unchecked>&)
{
   return a * b;
}

template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_divide(A a, A b, const std::integral_constant<int, checked>&)
{
   if (b == 0)
      raise_div_overflow();
   return a / b;
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_divide(A a, A b, const std::integral_constant<int, unchecked>&)
{
   return a / b;
}

template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_left_shift(A a, unsigned long long shift, const std::integral_constant<int, checked>&)
{
   if (a && shift)
   {
      if ((shift > sizeof(A) * CHAR_BIT) || (a >> (sizeof(A) * CHAR_BIT - shift)))
         BOOST_MP_THROW_EXCEPTION(std::overflow_error("Shift out of range"));
   }
   return a << shift;
}
template <class A>
inline BOOST_MP_CXX14_CONSTEXPR A checked_left_shift(A a, unsigned long long shift, const std::integral_constant<int, unchecked>&)
{
   return (shift >= sizeof(A) * CHAR_BIT) ? 0 : a << shift;
}

}}}} // namespace boost::multiprecision::backends::detail

#endif
