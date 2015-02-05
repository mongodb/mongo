//  Copyright 2010 John Maddock

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_TYPE_TRAITS_EXT_ADD_LVALUE_REFERENCE__HPP
#define BOOST_TYPE_TRAITS_EXT_ADD_LVALUE_REFERENCE__HPP

#include <boost/type_traits/add_reference.hpp>

// should be the last #include
#include <boost/type_traits/detail/type_trait_def.hpp>

namespace boost{

BOOST_TT_AUX_TYPE_TRAIT_DEF1(add_lvalue_reference,T,typename boost::add_reference<T>::type)

#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
BOOST_TT_AUX_TYPE_TRAIT_PARTIAL_SPEC1_1(typename T,add_lvalue_reference,T&&,T&)
#endif

}

#include <boost/type_traits/detail/type_trait_undef.hpp>

#endif  // BOOST_TYPE_TRAITS_EXT_ADD_LVALUE_REFERENCE__HPP
