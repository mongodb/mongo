
//  (C) Copyright John Maddock 2005.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_REMOVE_ALL_EXTENTS_HPP_INCLUDED
#define BOOST_TT_REMOVE_ALL_EXTENTS_HPP_INCLUDED

#include <boost/config.hpp>
#include <cstddef>
#include <boost/detail/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC,<=1300)
#include <boost/type_traits/msvc/remove_all_extents.hpp>
#endif

// should be the last #include
#include <boost/type_traits/detail/type_trait_def.hpp>

#if !BOOST_WORKAROUND(BOOST_MSVC,<=1300)

namespace boost {

BOOST_TT_AUX_TYPE_TRAIT_DEF1(remove_all_extents,T,T)

#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) && !defined(BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_all_extents,T[N],typename boost::remove_all_extents<T>::type type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_all_extents,T const[N],typename boost::remove_all_extents<T const>::type type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_all_extents,T volatile[N],typename boost::remove_all_extents<T volatile>::type type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_all_extents,T const volatile[N],typename boost::remove_all_extents<T const volatile>::type type)
#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x610)) && !defined(__IBMCPP__) &&  !BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x840))
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_all_extents,T[],typename boost::remove_all_extents<T>::type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_all_extents,T const[],typename boost::remove_all_extents<T const>::type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_all_extents,T volatile[],typename boost::remove_all_extents<T volatile>::type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_all_extents,T const volatile[],typename boost::remove_all_extents<T const volatile>::type)
#endif
#endif

} // namespace boost

#endif

#include <boost/type_traits/detail/type_trait_undef.hpp>

#endif // BOOST_TT_REMOVE_BOUNDS_HPP_INCLUDED
