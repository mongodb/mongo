//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2009-2012.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/move for documentation.
//
//////////////////////////////////////////////////////////////////////////////

//! \file

#ifndef BOOST_MOVE_MOVE_TRAITS_HPP
#define BOOST_MOVE_MOVE_TRAITS_HPP

#include <boost/move/detail/config_begin.hpp>
#include <boost/type_traits/has_trivial_destructor.hpp>
#include <boost/type_traits/is_nothrow_move_constructible.hpp>
#include <boost/type_traits/is_nothrow_move_assignable.hpp>
#include <boost/move/detail/meta_utils.hpp>

#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
#include <boost/move/core.hpp>
#endif

namespace boost {

//! If this trait yields to true
//! (<i>has_trivial_destructor_after_move &lt;T&gt;::value == true</i>)
//! means that if T is used as argument of a move construction/assignment,
//! there is no need to call T's destructor.
//! This optimization tipically is used to improve containers' performance.
//!
//! By default this trait is true if the type has trivial destructor,
//! every class should specialize this trait if it wants to improve performance
//! when inserted in containers.
template <class T>
struct has_trivial_destructor_after_move
   : ::boost::has_trivial_destructor<T>
{};

//! By default this traits returns
//! <pre>boost::is_nothrow_move_constructible<T>::value && boost::is_nothrow_move_assignable<T>::value </pre>.
//! Classes with non-throwing move constructor
//! and assignment can specialize this trait to obtain some performance improvements.
template <class T>
struct has_nothrow_move
   : public ::boost::move_detail::integral_constant
      < bool
      , boost::is_nothrow_move_constructible<T>::value &&
        boost::is_nothrow_move_assignable<T>::value
      >
{};

namespace move_detail {

// Code from Jeffrey Lee Hellrung, many thanks

#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
   template< class T> struct forward_type { typedef T type; };
#else // #ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
   template< class T>
   struct forward_type
   { typedef const T &type; };

   template< class T>
   struct forward_type< boost::rv<T> >
   { typedef T type; };
#endif // #ifndef BOOST_NO_CXX11_RVALUE_REFERENCES

template< class T > struct is_rvalue_reference : ::boost::move_detail::integral_constant<bool, false> { };
#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
   template< class T > struct is_rvalue_reference< T&& > : ::boost::move_detail::integral_constant<bool, true> { };
#else // #ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
   template< class T > struct is_rvalue_reference< boost::rv<T>& >
      :  ::boost::move_detail::integral_constant<bool, true>
   {};

   template< class T > struct is_rvalue_reference< const boost::rv<T>& >
      : ::boost::move_detail::integral_constant<bool, true>
   {};
#endif // #ifndef BOOST_NO_CXX11_RVALUE_REFERENCES

#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
   template< class T > struct add_rvalue_reference { typedef T&& type; };
#else // #ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
   namespace detail_add_rvalue_reference
   {
      template< class T
              , bool emulation = ::boost::has_move_emulation_enabled<T>::value
              , bool rv        = ::boost::move_detail::is_rv<T>::value  >
      struct add_rvalue_reference_impl { typedef T type; };

      template< class T, bool emulation>
      struct add_rvalue_reference_impl< T, emulation, true > { typedef T & type; };

      template< class T, bool rv >
      struct add_rvalue_reference_impl< T, true, rv > { typedef ::boost::rv<T>& type; };
   } // namespace detail_add_rvalue_reference

   template< class T >
   struct add_rvalue_reference
      : detail_add_rvalue_reference::add_rvalue_reference_impl<T>
   { };

   template< class T >
   struct add_rvalue_reference<T &>
   {  typedef T & type; };

#endif // #ifndef BOOST_NO_CXX11_RVALUE_REFERENCES

template< class T > struct remove_rvalue_reference { typedef T type; };

#ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
   template< class T > struct remove_rvalue_reference< T&& >                  { typedef T type; };
#else // #ifndef BOOST_NO_CXX11_RVALUE_REFERENCES
   template< class T > struct remove_rvalue_reference< rv<T> >                { typedef T type; };
   template< class T > struct remove_rvalue_reference< const rv<T> >          { typedef T type; };
   template< class T > struct remove_rvalue_reference< volatile rv<T> >       { typedef T type; };
   template< class T > struct remove_rvalue_reference< const volatile rv<T> > { typedef T type; };
   template< class T > struct remove_rvalue_reference< rv<T>& >               { typedef T type; };
   template< class T > struct remove_rvalue_reference< const rv<T>& >         { typedef T type; };
   template< class T > struct remove_rvalue_reference< volatile rv<T>& >      { typedef T type; };
   template< class T > struct remove_rvalue_reference< const volatile rv<T>& >{ typedef T type; };
#endif // #ifndef BOOST_NO_CXX11_RVALUE_REFERENCES

template <typename T>
typename boost::move_detail::add_rvalue_reference<T>::type declval();

}  //move_detail {

// Ideas from Boost.Move review, Jeffrey Lee Hellrung:
//
//- TypeTraits metafunctions is_lvalue_reference, add_lvalue_reference, and remove_lvalue_reference ?
//  Perhaps add_reference and remove_reference can be modified so that they behave wrt emulated rvalue
//  references the same as wrt real rvalue references, i.e., add_reference< rv<T>& > -> T& rather than
//  rv<T>& (since T&& & -> T&).
//
//- Add'l TypeTraits has_[trivial_]move_{constructor,assign}...?
//
//- An as_lvalue(T& x) function, which amounts to an identity operation in C++0x, but strips emulated
//  rvalue references in C++03.  This may be necessary to prevent "accidental moves".


}  //namespace boost {

#include <boost/move/detail/config_end.hpp>

#endif //#ifndef BOOST_MOVE_MOVE_TRAITS_HPP
