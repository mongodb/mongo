//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Pablo Halpern 2009. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2011-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_POINTER_TRAITS_HPP
#define BOOST_INTRUSIVE_POINTER_TRAITS_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/memory_util.hpp>
#include <boost/type_traits/integral_constant.hpp>
#include <cstddef>

namespace boost {
namespace intrusive {

//! pointer_traits is the implementation of C++11 std::pointer_traits class with some
//! extensions like castings.
//!
//! pointer_traits supplies a uniform interface to certain attributes of pointer-like types.
template <typename Ptr>
struct pointer_traits
{
   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
      //!The pointer type
      //!queried by this pointer_traits instantiation
      typedef Ptr             pointer;

      //!Ptr::element_type if such a type exists; otherwise, T if Ptr is a class
      //!template instantiation of the form SomePointer<T, Args>, where Args is zero or
      //!more type arguments ; otherwise , the specialization is ill-formed.
      typedef unspecified_type element_type;

      //!Ptr::difference_type if such a type exists; otherwise,
      //!std::ptrdiff_t.
      typedef unspecified_type difference_type;

      //!Ptr::rebind<U> if such a type exists; otherwise, SomePointer<U, Args> if Ptr is
      //!a class template instantiation of the form SomePointer<T, Args>, where Args is zero or
      //!more type arguments ; otherwise, the instantiation of rebind is ill-formed.
      //!
      //!For portable code for C++03 and C++11, <pre>typename rebind_pointer<U>::type</pre>
      //!shall be used instead of rebind<U> to obtain a pointer to U.
      template <class U> using rebind = unspecified;

      //!Ptr::rebind<U> if such a type exists; otherwise, SomePointer<U, Args> if Ptr is
      //!a class template instantiation of the form SomePointer<T, Args>, where Args is zero or
      //!more type arguments ; otherwise, the instantiation of rebind is ill-formed.
      //!
      typedef element_type &reference;
   #else
      typedef Ptr                                                             pointer;
      //
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_EVAL_DEFAULT
         ( boost::intrusive::detail::, Ptr, element_type
         , boost::intrusive::detail::first_param<Ptr>)                        element_type;
      //
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT
         (boost::intrusive::detail::, Ptr, difference_type, std::ptrdiff_t)   difference_type;
      //
      typedef typename boost::intrusive::detail::unvoid_ref<element_type>::type  reference;
      //
      template <class U> struct rebind_pointer
      {
         typedef typename boost::intrusive::detail::type_rebinder<Ptr, U>::type  type;
      };

      #if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
         template <class U> using rebind = typename boost::intrusive::detail::type_rebinder<Ptr, U>::type;
      #endif
   #endif   //#if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)

   //! <b>Remark</b>: If element_type is (possibly cv-qualified) void, r type is unspecified; otherwise,
   //!   it is element_type &.
   //!
   //! <b>Returns</b>: A dereferenceable pointer to r obtained by calling Ptr::pointer_to(r).
   //!   Non-standard extension: If such function does not exist, returns pointer(addressof(r));
   static pointer pointer_to(reference r)
   {
      //Non-standard extension, it does not require Ptr::pointer_to. If not present
      //tries to converts &r to pointer.
      const bool value = boost::intrusive::detail::
         has_member_function_callable_with_pointer_to
            <Ptr, typename boost::intrusive::detail::unvoid<element_type &>::type>::value;
      ::boost::integral_constant<bool, value> flag;
      return pointer_traits::priv_pointer_to(flag, r);
   }

   //! <b>Remark</b>: Non-standard extension.
   //!
   //! <b>Returns</b>: A dereferenceable pointer to r obtained by calling Ptr::static_cast_from(r).
   //!   If such function does not exist, returns pointer_to(static_cast<element_type&>(*uptr))
   template<class UPtr>
   static pointer static_cast_from(const UPtr &uptr)
   {
      const bool value = boost::intrusive::detail::
         has_member_function_callable_with_static_cast_from
            <Ptr, const UPtr>::value;
      ::boost::integral_constant<bool, value> flag;
      return pointer_traits::priv_static_cast_from(flag, uptr);
   }

   //! <b>Remark</b>: Non-standard extension.
   //!
   //! <b>Returns</b>: A dereferenceable pointer to r obtained by calling Ptr::const_cast_from(r).
   //!   If such function does not exist, returns pointer_to(const_cast<element_type&>(*uptr))
   template<class UPtr>
   static pointer const_cast_from(const UPtr &uptr)
   {
      const bool value = boost::intrusive::detail::
         has_member_function_callable_with_const_cast_from
            <Ptr, const UPtr>::value;
      ::boost::integral_constant<bool, value> flag;
      return pointer_traits::priv_const_cast_from(flag, uptr);
   }

   //! <b>Remark</b>: Non-standard extension.
   //!
   //! <b>Returns</b>: A dereferenceable pointer to r obtained by calling Ptr::dynamic_cast_from(r).
   //!   If such function does not exist, returns pointer_to(*dynamic_cast<element_type*>(&*uptr))
   template<class UPtr>
   static pointer dynamic_cast_from(const UPtr &uptr)
   {
      const bool value = boost::intrusive::detail::
         has_member_function_callable_with_dynamic_cast_from
            <Ptr, const UPtr>::value;
      ::boost::integral_constant<bool, value> flag;
      return pointer_traits::priv_dynamic_cast_from(flag, uptr);
   }

   ///@cond
   private:
   //priv_to_raw_pointer
   template <class T>
   static T* to_raw_pointer(T* p)
   {  return p; }

   template <class Pointer>
   static typename pointer_traits<Pointer>::element_type*
      to_raw_pointer(const Pointer &p)
   {  return pointer_traits::to_raw_pointer(p.operator->());  }

   //priv_pointer_to
   static pointer priv_pointer_to(boost::true_type, typename boost::intrusive::detail::unvoid<element_type>::type& r)
      { return Ptr::pointer_to(r); }

   static pointer priv_pointer_to(boost::false_type, typename boost::intrusive::detail::unvoid<element_type>::type& r)
      { return pointer(boost::intrusive::detail::addressof(r)); }

   //priv_static_cast_from
   template<class UPtr>
   static pointer priv_static_cast_from(boost::true_type, const UPtr &uptr)
   { return Ptr::static_cast_from(uptr); }

   template<class UPtr>
   static pointer priv_static_cast_from(boost::false_type, const UPtr &uptr)
   {  return pointer_to(*static_cast<element_type*>(to_raw_pointer(uptr)));  }

   //priv_const_cast_from
   template<class UPtr>
   static pointer priv_const_cast_from(boost::true_type, const UPtr &uptr)
   { return Ptr::const_cast_from(uptr); }

   template<class UPtr>
   static pointer priv_const_cast_from(boost::false_type, const UPtr &uptr)
   {  return pointer_to(const_cast<element_type&>(*uptr));  }

   //priv_dynamic_cast_from
   template<class UPtr>
   static pointer priv_dynamic_cast_from(boost::true_type, const UPtr &uptr)
   { return Ptr::dynamic_cast_from(uptr); }

   template<class UPtr>
   static pointer priv_dynamic_cast_from(boost::false_type, const UPtr &uptr)
   {  return pointer_to(*dynamic_cast<element_type*>(&*uptr));  }
   ///@endcond
};

///@cond

// Remove cv qualification from Ptr parameter to pointer_traits:
template <typename Ptr>
struct pointer_traits<const Ptr> : pointer_traits<Ptr> {};
template <typename Ptr>
struct pointer_traits<volatile Ptr> : pointer_traits<Ptr> { };
template <typename Ptr>
struct pointer_traits<const volatile Ptr> : pointer_traits<Ptr> { };
// Remove reference from Ptr parameter to pointer_traits:
template <typename Ptr>
struct pointer_traits<Ptr&> : pointer_traits<Ptr> { };

///@endcond

//! Specialization of pointer_traits for raw pointers
//!
template <typename T>
struct pointer_traits<T*>
{
   typedef T            element_type;
   typedef T*           pointer;
   typedef std::ptrdiff_t difference_type;

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
      typedef T &          reference;
      //!typedef for <pre>U *</pre>
      //!
      //!For portable code for C++03 and C++11, <pre>typename rebind_pointer<U>::type</pre>
      //!shall be used instead of rebind<U> to obtain a pointer to U.
      template <class U> using rebind = U*;
   #else
      typedef typename boost::intrusive::detail::unvoid_ref<element_type>::type reference;
      #if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
         template <class U> using rebind = U*;
      #endif
   #endif

   template <class U> struct rebind_pointer
   {  typedef U* type;  };

   //! <b>Returns</b>: addressof(r)
   //!
   static pointer pointer_to(reference r)
   { return boost::intrusive::detail::addressof(r); }

   //! <b>Returns</b>: static_cast<pointer>(uptr)
   //!
   template<class U>
   static pointer static_cast_from(U *uptr)
   {  return static_cast<pointer>(uptr);  }

   //! <b>Returns</b>: const_cast<pointer>(uptr)
   //!
   template<class U>
   static pointer const_cast_from(U *uptr)
   {  return const_cast<pointer>(uptr);  }

   //! <b>Returns</b>: dynamic_cast<pointer>(uptr)
   //!
   template<class U>
   static pointer dynamic_cast_from(U *uptr)
   {  return dynamic_cast<pointer>(uptr);  }
};

}  //namespace container {
}  //namespace boost {

#include <boost/intrusive/detail/config_end.hpp>

#endif // ! defined(BOOST_INTRUSIVE_POINTER_TRAITS_HPP)
