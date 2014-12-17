
//  Copyright 2000 John Maddock (john@johnmaddock.co.uk)
//  Copyright 2002 Aleksey Gurtovoy (agurtovoy@meta-comm.com)
//
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_IS_FUNCTION_HPP_INCLUDED
#define BOOST_TT_IS_FUNCTION_HPP_INCLUDED

#include <boost/type_traits/is_reference.hpp>
#include <boost/type_traits/detail/false_result.hpp>
#include <boost/config.hpp>

#if !defined(BOOST_TT_TEST_MS_FUNC_SIGS)
#   include <boost/type_traits/detail/is_function_ptr_helper.hpp>
#else
#   include <boost/type_traits/detail/is_function_ptr_tester.hpp>
#   include <boost/type_traits/detail/yes_no_type.hpp>
#endif

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

// is a type a function?
// Please note that this implementation is unnecessarily complex:
// we could just use !is_convertible<T*, const volatile void*>::value,
// except that some compilers erroneously allow conversions from
// function pointers to void*.

namespace boost {

#if !defined( __CODEGEARC__ )

namespace detail {

#if !defined(BOOST_TT_TEST_MS_FUNC_SIGS)
template<bool is_ref = true>
struct is_function_chooser
    : public ::boost::type_traits::false_result
{
};

template <>
struct is_function_chooser<false>
{
    template< typename T > struct result_
        : public ::boost::type_traits::is_function_ptr_helper<T*>
    {
    };
};

template <typename T>
struct is_function_impl
    : public is_function_chooser< ::boost::is_reference<T>::value >
        ::BOOST_NESTED_TEMPLATE result_<T>
{
};

#else

template <typename T>
struct is_function_impl
{
#if BOOST_WORKAROUND(BOOST_MSVC_FULL_VER, >= 140050000)
#pragma warning(push)
#pragma warning(disable:6334)
#endif
    static T* t;
    BOOST_STATIC_CONSTANT(
        bool, value = sizeof(::boost::type_traits::is_function_ptr_tester(t))
        == sizeof(::boost::type_traits::yes_type)
        );
#if BOOST_WORKAROUND(BOOST_MSVC_FULL_VER, >= 140050000)
#pragma warning(pop)
#endif
};

template <typename T>
struct is_function_impl<T&> : public false_type
{};
#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
template <typename T>
struct is_function_impl<T&&> : public false_type
{};
#endif

#endif

} // namespace detail

#endif // !defined( __CODEGEARC__ )

#if defined( __CODEGEARC__ )
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_function,T,__is_function(T))
#else
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_function,T,::boost::detail::is_function_impl<T>::value)
#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_function,T&&,false)
#endif
#endif
} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_FUNCTION_HPP_INCLUDED
