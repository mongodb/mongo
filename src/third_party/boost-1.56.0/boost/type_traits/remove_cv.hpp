
//  (C) Copyright Dave Abrahams, Steve Cleary, Beman Dawes, Howard
//  Hinnant & John Maddock 2000.  
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.


#ifndef BOOST_TT_REMOVE_CV_HPP_INCLUDED
#define BOOST_TT_REMOVE_CV_HPP_INCLUDED

#include <boost/type_traits/detail/cv_traits_impl.hpp>
#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#include <cstddef>

// should be the last #include
#include <boost/type_traits/detail/type_trait_def.hpp>

namespace boost {


namespace detail{

template <class T>
struct rvalue_ref_filter_rem_cv
{
   typedef typename boost::detail::cv_traits_imp<BOOST_TT_AUX_CV_TRAITS_IMPL_PARAM(T)>::unqualified_type type;
};

#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
//
// We can't filter out rvalue_references at the same level as
// references or we get ambiguities from msvc:
//
template <class T>
struct rvalue_ref_filter_rem_cv<T&&>
{
   typedef T&& type;
};
#endif

}


//  convert a type T to a non-cv-qualified type - remove_cv<T>
BOOST_TT_AUX_TYPE_TRAIT_DEF1(remove_cv,T,typename boost::detail::rvalue_ref_filter_rem_cv<T>::type)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,remove_cv,T&,T&)
#if !defined(BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS)
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_cv,T const[N],T type[N])
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_cv,T volatile[N],T type[N])
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_2(typename T,std::size_t N,remove_cv,T const volatile[N],T type[N])
#endif


} // namespace boost

#include <boost/type_traits/detail/type_trait_undef.hpp>

#endif // BOOST_TT_REMOVE_CV_HPP_INCLUDED
