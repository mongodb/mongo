
//  (C) Copyright Dave Abrahams, Steve Cleary, Beman Dawes, 
//      Howard Hinnant and John Maddock 2000. 
//  (C) Copyright Mat Marcus, Jesse Jones and Adobe Systems Inc 2001

//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

//    Fixed is_pointer, is_reference, is_const, is_volatile, is_same, 
//    is_member_pointer based on the Simulated Partial Specialization work 
//    of Mat Marcus and Jesse Jones. See  http://opensource.adobe.com or 
//    http://groups.yahoo.com/group/boost/message/5441 
//    Some workarounds in here use ideas suggested from "Generic<Programming>: 
//    Mappings between Types and Values" 
//    by Andrei Alexandrescu (see http://www.cuj.com/experts/1810/alexandr.html).


#ifndef BOOST_TT_IS_POINTER_HPP_INCLUDED
#define BOOST_TT_IS_POINTER_HPP_INCLUDED

#include <boost/type_traits/is_member_pointer.hpp>
#include <boost/type_traits/detail/ice_and.hpp>
#include <boost/type_traits/detail/ice_not.hpp>
#include <boost/type_traits/config.hpp>
#include <boost/type_traits/remove_cv.hpp>


// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

#if defined( __CODEGEARC__ )
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_pointer,T,__is_pointer(T))
#else

namespace detail {

template< typename T > struct is_pointer_helper
{
    BOOST_STATIC_CONSTANT(bool, value = false);
};

#   define TT_AUX_BOOL_TRAIT_HELPER_PARTIAL_SPEC(helper,sp,result) \
template< typename T > struct helper<sp> \
{ \
    BOOST_STATIC_CONSTANT(bool, value = result); \
}; \
/**/

TT_AUX_BOOL_TRAIT_HELPER_PARTIAL_SPEC(is_pointer_helper,T*,true)

#   undef TT_AUX_BOOL_TRAIT_HELPER_PARTIAL_SPEC

template< typename T >
struct is_pointer_impl
{
    BOOST_STATIC_CONSTANT(bool, value =
        (::boost::type_traits::ice_and<
        ::boost::detail::is_pointer_helper<typename remove_cv<T>::type>::value
            , ::boost::type_traits::ice_not<
                ::boost::is_member_pointer<T>::value
                >::value
            >::value)
        );
};

} // namespace detail

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_pointer,T,::boost::detail::is_pointer_impl<T>::value)

#if defined(__BORLANDC__) && !defined(__COMO__) && (__BORLANDC__ < 0x600)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_pointer,T&,false)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_pointer,T& const,false)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_pointer,T& volatile,false)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_pointer,T& const volatile,false)
#endif

#endif

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_POINTER_HPP_INCLUDED
