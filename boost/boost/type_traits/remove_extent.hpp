
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000-2005.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_REMOVE_EXTENT_HPP_INCLUDED
#define BOOST_TT_REMOVE_EXTENT_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>
#include <cstddef>

#if BOOST_WORKAROUND(BOOST_MSVC,<=1300)
#include <boost/type_traits/msvc/remove_extent.hpp>
#endif

// should be the last #include
#include <boost/type_traits/detail/type_trait_def.hpp>

#if !BOOST_WORKAROUND(BOOST_MSVC,<=1300)

namespace boost {

BOOST_TT_AUX_TYPE_TRAIT_DEF1(remove_extent,T,T)

#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) && !defined(BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_extent,T[N],T type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_extent,T const[N],T const type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_extent,T volatile[N],T volatile type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_extent,T const volatile[N],T const volatile type)
#if !BOOST_WORKAROUND(__BORLANDC__, < 0x600) && !defined(__IBMCPP__) &&  !BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x840))
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_extent,T[],T)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_extent,T const[],T const)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_extent,T volatile[],T volatile)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_extent,T const volatile[],T const volatile)
#endif
#endif

} // namespace boost

#endif

#include <boost/type_traits/detail/type_trait_undef.hpp>

#endif // BOOST_TT_REMOVE_BOUNDS_HPP_INCLUDED
