// Copyright (C) 2000 Stephen Cleary
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_POOL_CT_GCD_LCM_HPP
#define BOOST_POOL_CT_GCD_LCM_HPP

#include <boost/static_assert.hpp>
#include <boost/type_traits/ice.hpp>

namespace boost {

namespace details {
namespace pool {

// Compile-time calculation of greatest common divisor and least common multiple

//
// ct_gcd is a compile-time algorithm that calculates the greatest common
//  divisor of two unsigned integers, using Euclid's algorithm.
//
// assumes: A != 0 && B != 0
//

#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

namespace details {
template <unsigned A, unsigned B, bool Bis0>
struct ct_gcd_helper;
template <unsigned A, unsigned B>
struct ct_gcd_helper<A, B, false>
{
  BOOST_STATIC_CONSTANT(unsigned, A_mod_B_ = A % B);
  BOOST_STATIC_CONSTANT(unsigned, value =
      (::boost::details::pool::details::ct_gcd_helper<
        B, static_cast<unsigned>(A_mod_B_),
        ::boost::type_traits::ice_eq<A_mod_B_, 0>::value
        >::value) );
};
template <unsigned A, unsigned B>
struct ct_gcd_helper<A, B, true>
{
  BOOST_STATIC_CONSTANT(unsigned, value = A);
};
} // namespace details

template <unsigned A, unsigned B>
struct ct_gcd
{
  BOOST_STATIC_ASSERT(A != 0 && B != 0);
  BOOST_STATIC_CONSTANT(unsigned, value =
      (::boost::details::pool::details::ct_gcd_helper<A, B, false>::value) );
};

#else

// Thanks to Peter Dimov for providing this workaround!
namespace details {
template<unsigned A> struct ct_gcd2
{
  template<unsigned B>
  struct helper
  {
    BOOST_STATIC_CONSTANT(unsigned, value = ct_gcd2<B>::helper<A % B>::value);
  };
  template<>
  struct helper<0>
  {
    BOOST_STATIC_CONSTANT(unsigned, value = A);
  };
};
} // namespace details

template<unsigned A, unsigned B> struct ct_gcd
{
  BOOST_STATIC_ASSERT(A != 0 && B != 0);
  enum { value = details::ct_gcd2<A>::helper<B>::value };
};

#endif

//
// ct_lcm is a compile-time algorithm that calculates the least common
//  multiple of two unsigned integers.
//
// assumes: A != 0 && B != 0
//
template <unsigned A, unsigned B>
struct ct_lcm
{
  BOOST_STATIC_CONSTANT(unsigned, value =
      (A / ::boost::details::pool::ct_gcd<A, B>::value * B) );
};

} // namespace pool
} // namespace details

} // namespace boost

#endif
