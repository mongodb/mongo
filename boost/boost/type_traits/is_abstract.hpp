#ifndef BOOST_TT_IS_ABSTRACT_CLASS_HPP
#define BOOST_TT_IS_ABSTRACT_CLASS_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// is_abstract_class.hpp:
//
//  (C) Copyright 2002 Rani Sharoni (rani_sharoni@hotmail.com) and Robert Ramey
//  Use, modification and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//  
//  See http://www.boost.org for updates, documentation, and revision history.
//

// Compile type discovery whether given type is abstract class or not.
//
//   Requires DR 337 to be supported by compiler
//   (http://anubis.dkuug.dk/jtc1/sc22/wg21/docs/cwg_active.html#337).
//
//
// Believed (Jan 2004) to work on:
//  - GCC 3.4
//  - VC++ 7.1
//  - compilers with new EDG frontend (Intel C++ 7, Comeau 4.3.2)
//
// Doesn't work on:
//  - VC++6, VC++7.0 and less
//  - GCC 3.3.X and less
//  - Borland C++ 6 and less
//      
//
// History:
//  - Originally written by Rani Sharoni, see
//    http://groups.google.com/groups?selm=df893da6.0207110613.75b2fe90%40posting.google.com
//    At this time supported by EDG (Intel C++ 7, Comeau 4.3.2) and VC7.1.
//  - Adapted and added into Boost.Serialization library by Robert Ramey 
//    (starting with submission #10).
//  - Jan 2004: GCC 3.4 fixed to suport DR337 (Giovanni Bajo).
//  - Jan 2004: modified to be part of Boost.TypeTraits (Pavel Vozenilek).
//  - Nov 2004: Christoph Ludwig found that the implementation did not work with
//              template types and gcc-3.4 or VC7.1, fix due to Christoph Ludwig
//              and John Maddock.
//  - Dec 2004: Added new config macro BOOST_NO_IS_ABSTRACT which causes the template
//              to degrade gracefully, rather than trash the compiler (John Maddock).
//

#include <boost/static_assert.hpp>
#include <boost/type_traits/detail/yes_no_type.hpp>
#include <boost/type_traits/is_class.hpp>
#include <boost/type_traits/detail/ice_and.hpp>
#ifdef BOOST_NO_IS_ABSTRACT
#include <boost/type_traits/is_polymorphic.hpp>
#endif
// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>


namespace boost {
namespace detail{

#ifndef BOOST_NO_IS_ABSTRACT
template<class T>
struct is_abstract_imp2
{
   // Deduction fails if T is void, function type, 
   // reference type (14.8.2/2)or an abstract class type 
   // according to review status issue #337
   //
   template<class U>
   static type_traits::no_type check_sig(U (*)[1]);
   template<class U>
   static type_traits::yes_type check_sig(...);
   //
   // T must be a complete type, further if T is a template then
   // it must be instantiated in order for us to get the right answer:
   //
   BOOST_STATIC_ASSERT(sizeof(T) != 0);

   // GCC2 won't even parse this template if we embed the computation
   // of s1 in the computation of value.
#ifdef __GNUC__
   BOOST_STATIC_CONSTANT(unsigned, s1 = sizeof(is_abstract_imp2<T>::template check_sig<T>(0)));
#else
   BOOST_STATIC_CONSTANT(unsigned, s1 = sizeof(check_sig<T>(0)));
#endif
    
   BOOST_STATIC_CONSTANT(bool, value = 
      (s1 == sizeof(type_traits::yes_type)));
};

template <bool v>
struct is_abstract_select
{
   template <class T>
   struct rebind
   {
      typedef is_abstract_imp2<T> type;
   };
};
template <>
struct is_abstract_select<false>
{
   template <class T>
   struct rebind
   {
      typedef false_type type;
   };
};

template <class T>
struct is_abstract_imp
{
   typedef is_abstract_select< ::boost::is_class<T>::value> selector;
   typedef typename selector::template rebind<T> binder;
   typedef typename binder::type type;

   BOOST_STATIC_CONSTANT(bool, value = type::value);
};

#endif
}

#ifndef BOOST_NO_IS_ABSTRACT
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_abstract,T,::boost::detail::is_abstract_imp<T>::value)
#else
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_abstract,T,::boost::detail::is_polymorphic_imp<T>::value)
#endif

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif //BOOST_TT_IS_ABSTRACT_CLASS_HPP
