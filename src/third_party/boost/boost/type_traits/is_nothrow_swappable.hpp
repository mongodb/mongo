#ifndef BOOST_TYPE_TRAITS_IS_NOTHROW_SWAPPABLE_HPP_INCLUDED
#define BOOST_TYPE_TRAITS_IS_NOTHROW_SWAPPABLE_HPP_INCLUDED

//  Copyright 2017 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include <boost/config.hpp>

#if defined(BOOST_NO_SFINAE_EXPR) || defined(BOOST_NO_CXX11_NOEXCEPT) || defined(BOOST_NO_CXX11_DECLTYPE) \
   || defined(BOOST_NO_CXX11_FUNCTION_TEMPLATE_DEFAULT_ARGS)

#include <boost/type_traits/is_scalar.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/integral_constant.hpp>

namespace boost
{
template <class T> struct is_nothrow_swappable : boost::integral_constant<bool,
    boost::is_scalar<T>::value && !boost::is_const<T>::value> {};

template <class T, class U> struct is_nothrow_swappable_with : false_type {};
template <class T> struct is_nothrow_swappable_with<T, T> : is_nothrow_swappable<T> {};
}

#else

#include <boost/type_traits/detail/is_swappable_cxx_11.hpp>

namespace boost
{

template<class T, class U> struct is_nothrow_swappable_with: boost_type_traits_swappable_detail::is_nothrow_swappable_with_helper<T, U>::type
{
};

template<class T> struct is_nothrow_swappable: boost_type_traits_swappable_detail::is_nothrow_swappable_helper<T>::type
{
};

} // namespace boost

#endif

#endif // #ifndef BOOST_TYPE_TRAITS_IS_NOTHROW_SWAPPABLE_HPP_INCLUDED
