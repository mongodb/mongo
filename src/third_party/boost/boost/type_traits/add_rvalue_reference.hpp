//  add_rvalue_reference.hpp  ---------------------------------------------------------//

//  Copyright 2010 Vicente J. Botet Escriba

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_TYPE_TRAITS_EXT_ADD_RVALUE_REFERENCE__HPP
#define BOOST_TYPE_TRAITS_EXT_ADD_RVALUE_REFERENCE__HPP

#include <boost/config.hpp>

//----------------------------------------------------------------------------//

#include <boost/type_traits/is_void.hpp>
#include <boost/type_traits/is_reference.hpp>

// should be the last #include
#include <boost/type_traits/detail/type_trait_def.hpp>

//----------------------------------------------------------------------------//
//                                                                            //
//                           C++03 implementation of                          //
//             20.7.6.2 Reference modifications [meta.trans.ref]              //
//                          Written by Vicente J. Botet Escriba               //
//                                                                            //
// If T names an object or function type then the member typedef type
// shall name T&&; otherwise, type shall name T. [ Note: This rule reflects
// the semantics of reference collapsing. For example, when a type T names
// a type T1&, the type add_rvalue_reference<T>::type is not an rvalue
// reference. -end note ]
//----------------------------------------------------------------------------//

namespace boost {

namespace type_traits_detail {

    template <typename T, bool b>
    struct add_rvalue_reference_helper
    { typedef T   type; };

#if !defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
    template <typename T>
    struct add_rvalue_reference_helper<T, true>
    {
        typedef T&&   type;
    };
#endif

    template <typename T>
    struct add_rvalue_reference_imp
    { 
       typedef typename boost::type_traits_detail::add_rvalue_reference_helper
                  <T, (!is_void<T>::value && !is_reference<T>::value) >::type type; 
    };

}

BOOST_TT_AUX_TYPE_TRAIT_DEF1(add_rvalue_reference,T,typename boost::type_traits_detail::add_rvalue_reference_imp<T>::type)

}  // namespace boost

#include <boost/type_traits/detail/type_trait_undef.hpp>

#endif  // BOOST_TYPE_TRAITS_EXT_ADD_RVALUE_REFERENCE__HPP

