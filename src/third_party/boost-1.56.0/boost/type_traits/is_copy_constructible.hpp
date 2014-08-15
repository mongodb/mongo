//  (C) Copyright Antony Polukhin 2013.
//
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_IS_COPY_CONSTRUCTIBLE_HPP_INCLUDED
#define BOOST_TT_IS_COPY_CONSTRUCTIBLE_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/type_traits/detail/yes_no_type.hpp>
#include <boost/type_traits/is_base_and_derived.hpp>
#include <boost/type_traits/add_reference.hpp>
#include <boost/type_traits/is_rvalue_reference.hpp>
#include <boost/utility/declval.hpp>
#include <boost/noncopyable.hpp>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

namespace detail{

template <bool DerivedFromNoncopyable, class T>
struct is_copy_constructible_impl2 {

// Intel compiler has problems with SFINAE for copy constructors and deleted functions:
//
// error: function *function_name* cannot be referenced -- it is a deleted function
// static boost::type_traits::yes_type test(T1&, decltype(T1(boost::declval<T1&>()))* = 0);
//                                                        ^ 
#if !defined(BOOST_NO_CXX11_DELETED_FUNCTIONS) && !defined(BOOST_INTEL_CXX_VERSION)

#ifdef BOOST_NO_CXX11_DECLTYPE
    template <class T1>
    static boost::type_traits::yes_type test(T1&, boost::mpl::int_<sizeof(T1(boost::declval<T1&>()))>* = 0);
#else
    template <class T1>
    static boost::type_traits::yes_type test(T1&, decltype(T1(boost::declval<T1&>()))* = 0);
#endif

    static boost::type_traits::no_type test(...);
#else
    template <class T1>
    static boost::type_traits::no_type test(T1&, typename T1::boost_move_no_copy_constructor_or_assign* = 0);
    static boost::type_traits::yes_type test(...);
#endif

    // If you see errors like this:
    //
    //      `'T::T(const T&)' is private`
    //      `boost/type_traits/is_copy_constructible.hpp:68:5: error: within this context`
    //
    // then you are trying to call that macro for a structure defined like that:
    //
    //      struct T {
    //          ...
    //      private:
    //          T(const T &);
    //          ...
    //      };
    //
    // To fix that you must modify your structure:
    //
    //      // C++03 and C++11 version
    //      struct T: private boost::noncopyable {
    //          ...
    //      private:
    //          T(const T &);
    //          ...
    //      };
    //
    //      // C++11 version
    //      struct T {
    //          ...
    //      private:
    //          T(const T &) = delete;
    //          ...
    //      };
    BOOST_STATIC_CONSTANT(bool, value = (
            sizeof(test(
                boost::declval<BOOST_DEDUCED_TYPENAME boost::add_reference<T>::type>()
            )) == sizeof(boost::type_traits::yes_type)
        ||
            boost::is_rvalue_reference<T>::value
    ));
};

template <class T>
struct is_copy_constructible_impl2<true, T> {
    BOOST_STATIC_CONSTANT(bool, value = false);
};

template <class T>
struct is_copy_constructible_impl {

    BOOST_STATIC_CONSTANT(bool, value = (
        boost::detail::is_copy_constructible_impl2<
            boost::is_base_and_derived<boost::noncopyable, T>::value,
            T
        >::value
    ));
};

} // namespace detail

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_copy_constructible,T,::boost::detail::is_copy_constructible_impl<T>::value)
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_copy_constructible,void,false)
#ifndef BOOST_NO_CV_VOID_SPECIALIZATIONS
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_copy_constructible,void const,false)
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_copy_constructible,void const volatile,false)
BOOST_TT_AUX_BOOL_TRAIT_SPEC1(is_copy_constructible,void volatile,false)
#endif

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_COPY_CONSTRUCTIBLE_HPP_INCLUDED
