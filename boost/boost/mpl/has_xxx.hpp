
#ifndef BOOST_MPL_HAS_XXX_HPP_INCLUDED
#define BOOST_MPL_HAS_XXX_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2002-2006
// Copyright David Abrahams 2002-2003
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Source: /cvsroot/boost/boost/boost/mpl/has_xxx.hpp,v $
// $Date: 2006/11/09 01:05:31 $
// $Revision: 1.4.6.1 $

#include <boost/mpl/bool.hpp>
#include <boost/mpl/aux_/type_wrapper.hpp>
#include <boost/mpl/aux_/yes_no.hpp>
#include <boost/mpl/aux_/config/has_xxx.hpp>
#include <boost/mpl/aux_/config/msvc_typename.hpp>
#include <boost/mpl/aux_/config/msvc.hpp>
#include <boost/mpl/aux_/config/static_constant.hpp>
#include <boost/mpl/aux_/config/workaround.hpp>

#include <boost/preprocessor/cat.hpp>

#if !defined(BOOST_MPL_CFG_NO_HAS_XXX)

#   if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)

// agurt, 11/sep/02: MSVC-specific version (< 7.1), based on a USENET 
// newsgroup's posting by John Madsen (comp.lang.c++.moderated, 
// 1999-11-12 19:17:06 GMT); the code is _not_ standard-conforming, but 
// it works way more reliably than the SFINAE-based implementation

// Modified dwa 8/Oct/02 to handle reference types.

#   include <boost/mpl/if.hpp>
#   include <boost/mpl/bool.hpp>

namespace boost { namespace mpl { namespace aux {

struct has_xxx_tag;

#if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
template< typename U > struct msvc_incomplete_array
{
    typedef char (&type)[sizeof(U) + 1];
};
#endif

template< typename T >
struct msvc_is_incomplete
{
    // MSVC is capable of some kinds of SFINAE.  If U is an incomplete
    // type, it won't pick the second overload
    static char tester(...);

#if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
    template< typename U >
    static typename msvc_incomplete_array<U>::type tester(type_wrapper<U>);
#else
    template< typename U >
    static char (& tester(type_wrapper<U>) )[sizeof(U)+1];
#endif 
    
    BOOST_STATIC_CONSTANT(bool, value = 
          sizeof(tester(type_wrapper<T>())) == 1
        );
};

template<>
struct msvc_is_incomplete<int>
{
    BOOST_STATIC_CONSTANT(bool, value = false);
};

}}}

#   define BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF_(trait, name, default_) \
template< typename T, typename name = ::boost::mpl::aux::has_xxx_tag > \
struct BOOST_PP_CAT(trait,_impl) : T \
{ \
    static boost::mpl::aux::no_tag \
    test(void(*)(::boost::mpl::aux::has_xxx_tag)); \
    \
    static boost::mpl::aux::yes_tag test(...); \
    \
    BOOST_STATIC_CONSTANT(bool, value = \
          sizeof(test(static_cast<void(*)(name)>(0))) \
            != sizeof(boost::mpl::aux::no_tag) \
        ); \
    typedef boost::mpl::bool_<value> type; \
}; \
\
template< typename T, typename fallback_ = boost::mpl::bool_<default_> > \
struct trait \
    : boost::mpl::if_c< \
          boost::mpl::aux::msvc_is_incomplete<T>::value \
        , boost::mpl::bool_<false> \
        , BOOST_PP_CAT(trait,_impl)<T> \
        >::type \
{ \
}; \
\
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, void) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, bool) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, char) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, signed char) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, unsigned char) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, signed short) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, unsigned short) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, signed int) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, unsigned int) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, signed long) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, unsigned long) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, float) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, double) \
BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, long double) \
/**/

#   define BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, T) \
template<> struct trait<T> \
{ \
    BOOST_STATIC_CONSTANT(bool, value = false); \
    typedef boost::mpl::bool_<false> type; \
}; \
/**/

#if !defined(BOOST_NO_INTRINSIC_WCHAR_T)
#   define BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF(trait, name, unused) \
    BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF_(trait, name, unused) \
    BOOST_MPL_AUX_HAS_XXX_TRAIT_SPEC(trait, wchar_t) \
/**/
#else
#   define BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF(trait, name, unused) \
    BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF_(trait, name, unused) \
/**/
#endif


// SFINAE-based implementations below are derived from a USENET newsgroup's 
// posting by Rani Sharoni (comp.lang.c++.moderated, 2002-03-17 07:45:09 PST)

#   elif BOOST_WORKAROUND(BOOST_MSVC, BOOST_TESTED_AT(1400)) \
      || BOOST_WORKAROUND(__IBMCPP__, <= 700)

// MSVC 7.1+ & VACPP

// agurt, 15/jun/05: replace overload-based SFINAE implementation with SFINAE
// applied to partial specialization to fix some apparently random failures 
// (thanks to Daniel Wallin for researching this!)

namespace boost { namespace mpl { namespace aux {
template< typename T > struct msvc71_sfinae_helper { typedef void type; };
}}}

#   define BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF(trait, name, default_) \
template< typename T, typename U = void > \
struct BOOST_PP_CAT(trait,_impl_) \
{ \
    BOOST_STATIC_CONSTANT(bool, value = false); \
    typedef boost::mpl::bool_<value> type; \
}; \
\
template< typename T > \
struct BOOST_PP_CAT(trait,_impl_)< \
      T \
    , typename boost::mpl::aux::msvc71_sfinae_helper< typename T::name >::type \
    > \
{ \
    BOOST_STATIC_CONSTANT(bool, value = true); \
    typedef boost::mpl::bool_<value> type; \
}; \
\
template< typename T, typename fallback_ = boost::mpl::bool_<default_> > \
struct trait \
    : BOOST_PP_CAT(trait,_impl_)<T> \
{ \
}; \
/**/

#   else // other SFINAE-capable compilers

#   define BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF(trait, name, default_) \
template< typename T, typename fallback_ = boost::mpl::bool_<default_> > \
struct trait \
{ \
    struct gcc_3_2_wknd \
    { \
        template< typename U > \
        static boost::mpl::aux::yes_tag test( \
              boost::mpl::aux::type_wrapper<U> const volatile* \
            , boost::mpl::aux::type_wrapper<BOOST_MSVC_TYPENAME U::name>* = 0 \
            ); \
    \
        static boost::mpl::aux::no_tag test(...); \
    }; \
    \
    typedef boost::mpl::aux::type_wrapper<T> t_; \
    BOOST_STATIC_CONSTANT(bool, value = \
          sizeof(gcc_3_2_wknd::test(static_cast<t_*>(0))) \
            == sizeof(boost::mpl::aux::yes_tag) \
        ); \
    typedef boost::mpl::bool_<value> type; \
}; \
/**/

#   endif // BOOST_WORKAROUND(BOOST_MSVC, <= 1300)


#else // BOOST_MPL_CFG_NO_HAS_XXX

// placeholder implementation

#   define BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF(trait, name, default_) \
template< typename T, typename fallback_ = boost::mpl::bool_<default_> > \
struct trait \
{ \
    BOOST_STATIC_CONSTANT(bool, value = fallback_::value); \
    typedef fallback_ type; \
}; \
/**/

#endif

#define BOOST_MPL_HAS_XXX_TRAIT_DEF(name) \
    BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF(BOOST_PP_CAT(has_,name), name, false) \
/**/

#endif // BOOST_MPL_HAS_XXX_HPP_INCLUDED
