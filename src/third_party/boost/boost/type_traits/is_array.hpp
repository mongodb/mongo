
//  (C) Copyright Dave Abrahams, Steve Cleary, Beman Dawes, Howard
//  Hinnant & John Maddock 2000.  
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.


// Some fixes for is_array are based on a newgroup posting by Jonathan Lundquist.


#ifndef BOOST_TT_IS_ARRAY_HPP_INCLUDED
#define BOOST_TT_IS_ARRAY_HPP_INCLUDED

#include <boost/type_traits/config.hpp>

#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#   include <boost/type_traits/detail/yes_no_type.hpp>
#   include <boost/type_traits/detail/wrap.hpp>
#endif

#include <cstddef>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

#if defined( __CODEGEARC__ )
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_array,T,__is_array(T))
#elif !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_array,T,false)
#if !defined(BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,is_array,T[N],true)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,is_array,T const[N],true)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,is_array,T volatile[N],true)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,is_array,T const volatile[N],true)
#if !BOOST_WORKAROUND(__BORLANDC__, < 0x600) && !defined(__IBMCPP__) &&  !BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x840))
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_array,T[],true)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_array,T const[],true)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_array,T volatile[],true)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_array,T const volatile[],true)
#endif
#endif

#else // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

namespace detail {

using ::boost::type_traits::yes_type;
using ::boost::type_traits::no_type;
using ::boost::type_traits::wrap;

template< typename T > T(* is_array_tester1(wrap<T>) )(wrap<T>);
char BOOST_TT_DECL is_array_tester1(...);

template< typename T> no_type is_array_tester2(T(*)(wrap<T>));
yes_type BOOST_TT_DECL is_array_tester2(...);

template< typename T >
struct is_array_impl
{ 
    BOOST_STATIC_CONSTANT(bool, value = 
        sizeof(::boost::detail::is_array_tester2(
            ::boost::detail::is_array_tester1(
                ::boost::type_traits::wrap<T>()
                )
        )) == 1
    );
};

#ifndef BOOST_NO_CV_VOID_SPECIALIZATIONS
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_array,void,false)
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_array,void const,false)
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_array,void volatile,false)
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_array,void const volatile,false)
#endif

} // namespace detail

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_array,T,::boost::detail::is_array_impl<T>::value)

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_ARRAY_HPP_INCLUDED
