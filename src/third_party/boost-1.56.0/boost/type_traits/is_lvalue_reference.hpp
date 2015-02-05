
//  (C) Copyright Dave Abrahams, Steve Cleary, Beman Dawes, 
//      Howard Hinnant and John Maddock 2000. 
//  (C) Copyright Mat Marcus, Jesse Jones and Adobe Systems Inc 2001

//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

//    Fixed is_pointer, is_lvalue_reference, is_const, is_volatile, is_same, 
//    is_member_pointer based on the Simulated Partial Specialization work 
//    of Mat Marcus and Jesse Jones. See  http://opensource.adobe.com or 
//    http://groups.yahoo.com/group/boost/message/5441 
//    Some workarounds in here use ideas suggested from "Generic<Programming>: 
//    Mappings between Types and Values" 
//    by Andrei Alexandrescu (see http://www.cuj.com/experts/1810/alexandr.html).


#ifndef BOOST_TT_IS_LVALUE_REFERENCE_HPP_INCLUDED
#define BOOST_TT_IS_LVALUE_REFERENCE_HPP_INCLUDED

#include <boost/type_traits/config.hpp>


// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

#if defined( __CODEGEARC__ )
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_lvalue_reference,T,__is_reference(T))
#else

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_lvalue_reference,T,false)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_lvalue_reference,T&,true)

#if  defined(BOOST_ILLEGAL_CV_REFERENCES)
// these are illegal specialisations; cv-qualifies applied to
// references have no effect according to [8.3.2p1],
// C++ Builder requires them though as it treats cv-qualified
// references as distinct types...
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_lvalue_reference,T& const,true)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_lvalue_reference,T& volatile,true)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_lvalue_reference,T& const volatile,true)
#endif

#endif

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_REFERENCE_HPP_INCLUDED

