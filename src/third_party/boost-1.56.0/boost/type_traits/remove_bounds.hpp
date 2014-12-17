
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_REMOVE_BOUNDS_HPP_INCLUDED
#define BOOST_TT_REMOVE_BOUNDS_HPP_INCLUDED

#include <boost/config.hpp>
#include <cstddef>
#include <boost/detail/workaround.hpp>

// should be the last #include
#include <boost/type_traits/detail/type_trait_def.hpp>

namespace boost {

BOOST_TT_AUX_TYPE_TRAIT_DEF1(remove_bounds,T,T)

#if !defined(BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_bounds,T[N],T type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_bounds,T const[N],T const type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_bounds,T volatile[N],T volatile type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_bounds,T const volatile[N],T const volatile type)
#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x610)) && !defined(__IBMCPP__) &&  !BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x840))
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_bounds,T[],T)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_bounds,T const[],T const)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_bounds,T volatile[],T volatile)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_bounds,T const volatile[],T const volatile)
#endif
#endif

} // namespace boost

#include <boost/type_traits/detail/type_trait_undef.hpp>

#endif // BOOST_TT_REMOVE_BOUNDS_HPP_INCLUDED
