
// (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_IS_EMPTY_HPP_INCLUDED
#define BOOST_TT_IS_EMPTY_HPP_INCLUDED

#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/detail/ice_or.hpp>
#include <boost/type_traits/config.hpp>
#include <boost/type_traits/intrinsics.hpp>

#   include <boost/type_traits/remove_cv.hpp>
#   include <boost/type_traits/is_class.hpp>
#   include <boost/type_traits/add_reference.hpp>

// should be always the last #include directive
#include <boost/type_traits/detail/bool_trait_def.hpp>

#ifndef BOOST_INTERNAL_IS_EMPTY
#define BOOST_INTERNAL_IS_EMPTY(T) false
#else
#define BOOST_INTERNAL_IS_EMPTY(T) BOOST_IS_EMPTY(T)
#endif

namespace boost {

namespace detail {


#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4624) // destructor could not be generated
#endif

template <typename T>
struct empty_helper_t1 : public T
{
    empty_helper_t1();  // hh compiler bug workaround
    int i[256];
private:
   // suppress compiler warnings:
   empty_helper_t1(const empty_helper_t1&);
   empty_helper_t1& operator=(const empty_helper_t1&);
};

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

struct empty_helper_t2 { int i[256]; };

#if !BOOST_WORKAROUND(__BORLANDC__, < 0x600)

template <typename T, bool is_a_class = false>
struct empty_helper
{
    BOOST_STATIC_CONSTANT(bool, value = false);
};

template <typename T>
struct empty_helper<T, true>
{
    BOOST_STATIC_CONSTANT(
        bool, value = (sizeof(empty_helper_t1<T>) == sizeof(empty_helper_t2))
        );
};

template <typename T>
struct is_empty_impl
{
    typedef typename remove_cv<T>::type cvt;
    BOOST_STATIC_CONSTANT(
        bool, value = (
            ::boost::type_traits::ice_or<
              ::boost::detail::empty_helper<cvt,::boost::is_class<T>::value>::value
              , BOOST_INTERNAL_IS_EMPTY(cvt)
            >::value
            ));
};

#else // __BORLANDC__

template <typename T, bool is_a_class, bool convertible_to_int>
struct empty_helper
{
    BOOST_STATIC_CONSTANT(bool, value = false);
};

template <typename T>
struct empty_helper<T, true, false>
{
    BOOST_STATIC_CONSTANT(bool, value = (
        sizeof(empty_helper_t1<T>) == sizeof(empty_helper_t2)
        ));
};

template <typename T>
struct is_empty_impl
{
   typedef typename remove_cv<T>::type cvt;
   typedef typename add_reference<T>::type r_type;

   BOOST_STATIC_CONSTANT(
       bool, value = (
           ::boost::type_traits::ice_or<
              ::boost::detail::empty_helper<
                  cvt
                , ::boost::is_class<T>::value
                , ::boost::is_convertible< r_type,int>::value
              >::value
              , BOOST_INTERNAL_IS_EMPTY(cvt)
           >::value));
};

#endif // __BORLANDC__


// these help when the compiler has no partial specialization support:
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_empty,void,false)
#ifndef BOOST_NO_CV_VOID_SPECIALIZATIONS
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_empty,void const,false)
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_empty,void volatile,false)
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_empty,void const volatile,false)
#endif

} // namespace detail

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_empty,T,::boost::detail::is_empty_impl<T>::value)

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#undef BOOST_INTERNAL_IS_EMPTY

#endif // BOOST_TT_IS_EMPTY_HPP_INCLUDED

