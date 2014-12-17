
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_REMOVE_REFERENCE_HPP_INCLUDED
#define BOOST_TT_REMOVE_REFERENCE_HPP_INCLUDED

#include <boost/type_traits/broken_compiler_spec.hpp>
#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC,<=1300)
#include <boost/type_traits/msvc/remove_reference.hpp>
#endif

// should be the last #include
#include <boost/type_traits/detail/type_trait_def.hpp>

namespace boost {

#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

namespace detail{
//
// We can't filter out rvalue_references at the same level as
// references or we get ambiguities from msvc:
//
template <class T>
struct remove_rvalue_ref
{
   typedef T type;
};
#ifndef BOOST_NO_RVALUE_REFERENCES
template <class T>
struct remove_rvalue_ref<T&&>
{
   typedef T type;
};
#endif

} // namespace detail

BOOST_TT_AUX_TYPE_TRAIT_DEF1(remove_reference,T,typename boost::detail::remove_rvalue_ref<T>::type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_reference,T&,T)

#if defined(BOOST_ILLEGAL_CV_REFERENCES)
// these are illegal specialisations; cv-qualifies applied to
// references have no effect according to [8.3.2p1],
// C++ Builder requires them though as it treats cv-qualified
// references as distinct types...
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_reference,T& const,T)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_reference,T& volatile,T)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_reference,T& const volatile,T)
#endif

#elif !BOOST_WORKAROUND(BOOST_MSVC,<=1300)

BOOST_TT_AUX_TYPE_TRAIT_DEF1(remove_reference,T,typename boost::detail::remove_reference_impl<T>::type)

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

} // namespace boost

#include <boost/type_traits/detail/type_trait_undef.hpp>

#endif // BOOST_TT_REMOVE_REFERENCE_HPP_INCLUDED
