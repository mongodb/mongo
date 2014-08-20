
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  (C) Copyright Eric Friedman 2002-2003.
//  (C) Copyright Antony Polukhin 2013.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_IS_NOTHROW_MOVE_CONSTRUCTIBLE_HPP_INCLUDED
#define BOOST_TT_IS_NOTHROW_MOVE_CONSTRUCTIBLE_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/type_traits/has_trivial_move_constructor.hpp>
#include <boost/type_traits/has_nothrow_copy.hpp>
#include <boost/type_traits/is_array.hpp>
#include <boost/type_traits/is_reference.hpp>
#include <boost/type_traits/detail/ice_or.hpp>
#include <boost/type_traits/detail/ice_and.hpp>
#include <boost/utility/declval.hpp>
#include <boost/utility/enable_if.hpp>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

namespace detail{

#ifndef BOOST_NO_CXX11_NOEXCEPT

template <class T, class Enable = void>
struct false_or_cpp11_noexcept_move_constructible: public ::boost::false_type {};

template <class T>
struct false_or_cpp11_noexcept_move_constructible <
        T,
        typename ::boost::enable_if_c<sizeof(T) && BOOST_NOEXCEPT_EXPR(T(::boost::declval<T>()))>::type
    > : public ::boost::integral_constant<bool, BOOST_NOEXCEPT_EXPR(T(::boost::declval<T>()))>
{};

template <class T>
struct is_nothrow_move_constructible_imp{
   BOOST_STATIC_CONSTANT(bool, value = 
        (::boost::type_traits::ice_and<
            ::boost::type_traits::ice_not< ::boost::is_volatile<T>::value >::value,
            ::boost::type_traits::ice_not< ::boost::is_reference<T>::value >::value,
            ::boost::detail::false_or_cpp11_noexcept_move_constructible<T>::value
        >::value));
};

#else

template <class T>
struct is_nothrow_move_constructible_imp{
    BOOST_STATIC_CONSTANT(bool, value =(
        ::boost::type_traits::ice_and<
            ::boost::type_traits::ice_or<
                ::boost::has_trivial_move_constructor<T>::value,
                ::boost::has_nothrow_copy<T>::value
            >::value,
            ::boost::type_traits::ice_not< ::boost::is_array<T>::value >::value
        >::value));
};

#endif

#ifdef BOOST_NO_NOEXCEPT
//
// The above logic doesn't quite work in the absense of noexcept, 
// this is really to improve things with VC13:
//
template <class T>
struct is_nothrow_move_constructible_imp<T&>{ BOOST_STATIC_CONSTANT(bool, value = false); };
#endif

}

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_nothrow_move_constructible,T,::boost::detail::is_nothrow_move_constructible_imp<T>::value)

BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_nothrow_move_constructible,void,false)
#ifndef BOOST_NO_CV_VOID_SPECIALIZATIONS
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_nothrow_move_constructible,void const,false)
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_nothrow_move_constructible,void const volatile,false)
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_nothrow_move_constructible,void volatile,false)
#endif

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_NOTHROW_MOVE_CONSTRUCTIBLE_HPP_INCLUDED
