
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_IS_VOID_HPP_INCLUDED
#define BOOST_TT_IS_VOID_HPP_INCLUDED

#include <boost/config.hpp>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

//* is a type T void - is_void<T>
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_void,T,false)
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_void,void,true)

#ifndef BOOST_NO_CV_VOID_SPECIALIZATIONS
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_void,void const,true)
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_void,void volatile,true)
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_void,void const volatile,true)
#endif

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_VOID_HPP_INCLUDED
