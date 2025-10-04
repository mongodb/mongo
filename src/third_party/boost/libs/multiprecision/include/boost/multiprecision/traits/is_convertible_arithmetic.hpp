///////////////////////////////////////////////////////////////////////////////
//  Copyright 2021 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_IS_CONVERTIBLE_ARITHMETIC_HPP
#define BOOST_IS_CONVERTIBLE_ARITHMETIC_HPP

#include <type_traits>
#include <boost/multiprecision/detail/number_base.hpp>
#include <boost/multiprecision/detail/standalone_config.hpp>

namespace boost { namespace multiprecision { namespace detail {

template <class V, class Backend>
struct is_convertible_arithmetic
{
   static constexpr bool value = boost::multiprecision::detail::is_arithmetic<V>::value;
};
//
// For extension types, we don't *require* interoperability, 
// so only enable it if we can convert the type to the backend
// losslessly, ie not via conversion to a narrower type.
// Note that backends with templated constructors/=operators
// will not be selected here, so these need to either specialize
// this trait, or provide a proper non-template constructor/=operator
// for the extension types it supports.
//
#ifdef BOOST_HAS_FLOAT128
template <class Backend>
struct is_convertible_arithmetic<float128_type, Backend>
{
   static constexpr bool value = std::is_assignable<Backend, convertible_to<float128_type>>::value;
};
#endif
#ifdef BOOST_HAS_INT128
template <class Backend>
struct is_convertible_arithmetic<int128_type, Backend>
{
   static constexpr bool value = std::is_assignable<Backend, convertible_to<int128_type>>::value;
};
template <class Backend>
struct is_convertible_arithmetic<uint128_type, Backend>
{
   static constexpr bool value = std::is_assignable<Backend, convertible_to<uint128_type>>::value;
};
#endif

}}} // namespace boost::multiprecision::detail

#endif // BOOST_IS_BYTE_CONTAINER_HPP
