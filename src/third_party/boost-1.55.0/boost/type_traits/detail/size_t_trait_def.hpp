
// NO INCLUDE GUARDS, THE HEADER IS INTENDED FOR MULTIPLE INCLUSION

// Copyright Aleksey Gurtovoy 2002-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)

// $Source$
// $Date: 2011-04-25 05:26:48 -0700 (Mon, 25 Apr 2011) $
// $Revision: 71481 $

#include <boost/type_traits/detail/template_arity_spec.hpp>
#include <boost/type_traits/integral_constant.hpp>
#include <boost/mpl/aux_/lambda_support.hpp>
#include <boost/mpl/size_t.hpp>

#include <cstddef>

#if !defined(BOOST_MSVC) || BOOST_MSVC >= 1300
#   define BOOST_TT_AUX_SIZE_T_BASE(C) public ::boost::integral_constant<std::size_t,C>
#   define BOOST_TT_AUX_SIZE_T_TRAIT_VALUE_DECL(C) /**/
#else
#   define BOOST_TT_AUX_SIZE_T_BASE(C) public ::boost::mpl::size_t<C>
#   define BOOST_TT_AUX_SIZE_T_TRAIT_VALUE_DECL(C) \
    typedef ::boost::mpl::size_t<C> base_; \
    using base_::value; \
    /**/
#endif


#define BOOST_TT_AUX_SIZE_T_TRAIT_DEF1(trait,T,C) \
template< typename T > struct trait \
    : BOOST_TT_AUX_SIZE_T_BASE(C) \
{ \
public:\
    BOOST_TT_AUX_SIZE_T_TRAIT_VALUE_DECL(C) \
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,trait,(T)) \
}; \
\
BOOST_TT_AUX_TEMPLATE_ARITY_SPEC(1,trait) \
/**/

#define BOOST_TT_AUX_SIZE_T_TRAIT_SPEC1(trait,spec,C) \
template<> struct trait<spec> \
    : BOOST_TT_AUX_SIZE_T_BASE(C) \
{ \
public:\
    BOOST_TT_AUX_SIZE_T_TRAIT_VALUE_DECL(C) \
    BOOST_MPL_AUX_LAMBDA_SUPPORT_SPEC(1,trait,(spec)) \
}; \
/**/

#define BOOST_TT_AUX_SIZE_T_TRAIT_PARTIAL_SPEC1_1(param,trait,spec,C) \
template< param > struct trait<spec> \
    : BOOST_TT_AUX_SIZE_T_BASE(C) \
{ \
}; \
/**/
