
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_IS_COMPOUND_HPP_INCLUDED
#define BOOST_TT_IS_COMPOUND_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/type_traits/is_fundamental.hpp>
#include <boost/type_traits/detail/ice_not.hpp>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

#if !defined( __CODEGEARC__ )
namespace detail {

template <typename T>
struct is_compound_impl
{
   BOOST_STATIC_CONSTANT(bool, value =
      (::boost::type_traits::ice_not<
         ::boost::is_fundamental<T>::value
       >::value));
};

} // namespace detail
#endif // !defined( __CODEGEARC__ )

#if defined( __CODEGEARC__ )
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_compound,T,__is_compound(T))
#else
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_compound,T,::boost::detail::is_compound_impl<T>::value)
#endif

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_COMPOUND_HPP_INCLUDED
