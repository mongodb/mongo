
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


#ifndef BOOST_TT_IS_CONST_HPP_INCLUDED
#define BOOST_TT_IS_CONST_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#   include <boost/type_traits/detail/cv_traits_impl.hpp>
#   ifdef __GNUC__
#       include <boost/type_traits/is_reference.hpp>
#   endif
#   if BOOST_WORKAROUND(BOOST_MSVC, < 1400)
#       include <boost/type_traits/remove_bounds.hpp>
#   endif
#else
#   include <boost/type_traits/is_reference.hpp>
#   include <boost/type_traits/is_array.hpp>
#   include <boost/type_traits/detail/yes_no_type.hpp>
#   include <boost/type_traits/detail/false_result.hpp>
#endif

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

#if defined( __CODEGEARC__ )

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_const,T,__is_const(T))

#elif !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)

namespace detail{
//
// We can't filter out rvalue_references at the same level as
// references or we get ambiguities from msvc:
//
template <class T>
struct is_const_rvalue_filter
{
#if BOOST_WORKAROUND(BOOST_MSVC, < 1400)
   BOOST_STATIC_CONSTANT(bool, value = ::boost::detail::cv_traits_imp<typename boost::remove_bounds<T>::type*>::is_const);
#else
   BOOST_STATIC_CONSTANT(bool, value = ::boost::detail::cv_traits_imp<T*>::is_const);
#endif
};
#ifndef BOOST_NO_RVALUE_REFERENCES
template <class T>
struct is_const_rvalue_filter<T&&>
{
   BOOST_STATIC_CONSTANT(bool, value = false);
};
#endif
}

//* is a type T  declared const - is_const<T>
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_const,T,::boost::detail::is_const_rvalue_filter<T>::value)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_const,T&,false)

#if  defined(BOOST_ILLEGAL_CV_REFERENCES)
// these are illegal specialisations; cv-qualifies applied to
// references have no effect according to [8.3.2p1],
// C++ Builder requires them though as it treats cv-qualified
// references as distinct types...
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_const,T& const,false)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_const,T& volatile,false)
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_const,T& const volatile,false)
#endif

#if defined(__GNUC__) && (__GNUC__ < 3)
// special case for gcc where illegally cv-qualified reference types can be
// generated in some corner cases:
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_const,T const,!(::boost::is_reference<T>::value))
BOOST_TT_AUX_BOOL_TRAIT_PARTIAL_SPEC1_1(typename T,is_const,T volatile const,!(::boost::is_reference<T>::value))
#endif

#else

namespace detail {

using ::boost::type_traits::yes_type;
using ::boost::type_traits::no_type;

yes_type is_const_tester(const volatile void*);
no_type is_const_tester(volatile void *);

template <bool is_ref, bool array>
struct is_const_helper
    : public ::boost::type_traits::false_result
{
};

template <>
struct is_const_helper<false,false>
{
    template <typename T> struct result_
    {
        static T* t;
        BOOST_STATIC_CONSTANT(bool, value = (
            sizeof(boost::detail::yes_type) == sizeof(boost::detail::is_const_tester(t))
            ));
    };
};

template <>
struct is_const_helper<false,true>
{
    template <typename T> struct result_
    {
        static T t;
        BOOST_STATIC_CONSTANT(bool, value = (
            sizeof(boost::detail::yes_type) == sizeof(boost::detail::is_const_tester(&t))
            ));
    };
};

template <typename T>
struct is_const_impl
    : public is_const_helper<
          is_reference<T>::value
        , is_array<T>::value
        >::template result_<T>
{
};

BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_const,void,false)
#ifndef BOOST_NO_CV_VOID_SPECIALIZATIONS
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_const,void const,true)
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_const,void volatile,false)
BOOST_TT_AUX_BOOL_TRAIT_IMPL_SPEC1(is_const,void const volatile,true)
#endif

} // namespace detail

//* is a type T  declared const - is_const<T>
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_const,T,::boost::detail::is_const_impl<T>::value)

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_CONST_HPP_INCLUDED

