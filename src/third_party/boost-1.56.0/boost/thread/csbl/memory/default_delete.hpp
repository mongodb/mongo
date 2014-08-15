// Copyright (C) 2013 Vicente J. Botet Escriba
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// 2013/10 Vicente J. Botet Escriba
//   Creation.

#ifndef BOOST_CSBL_MEMORY_DEFAULT_DELETE_HPP
#define BOOST_CSBL_MEMORY_DEFAULT_DELETE_HPP

#include <boost/thread/csbl/memory/config.hpp>

// 20.8.1 class template unique_ptr:
// default_delete

#if defined BOOST_NO_CXX11_SMART_PTR
#include <boost/thread/csbl/memory/pointer_traits.hpp>
#include <boost/type_traits/remove_cv.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/is_scalar.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/static_assert.hpp>

namespace boost
{
  namespace csbl
  {
    namespace detail
    {

      template <class _Ptr1, class _Ptr2,
      bool = is_same<typename remove_cv<typename pointer_traits<_Ptr1>::element_type>::type,
      typename remove_cv<typename pointer_traits<_Ptr2>::element_type>::type
      >::value
      >
      struct same_or_less_cv_qualified_imp
      : is_convertible<_Ptr1, _Ptr2>
      {};

      template <class _Ptr1, class _Ptr2>
      struct same_or_less_cv_qualified_imp<_Ptr1, _Ptr2, false>
      : false_type
      {};

      template <class _Ptr1, class _Ptr2, bool = is_scalar<_Ptr1>::value &&
      !is_pointer<_Ptr1>::value>
      struct same_or_less_cv_qualified
      : same_or_less_cv_qualified_imp<_Ptr1, _Ptr2>
      {};

      template <class _Ptr1, class _Ptr2>
      struct same_or_less_cv_qualified<_Ptr1, _Ptr2, true>
      : false_type
      {};

    }
    template <class T>
    struct BOOST_SYMBOL_VISIBLE default_delete
    {
#ifndef BOOST_NO_CXX11_DEFAULTED_FUNCTIONS
      BOOST_SYMBOL_VISIBLE
      BOOST_CONSTEXPR default_delete() = default;
#else
      BOOST_SYMBOL_VISIBLE
      BOOST_CONSTEXPR default_delete() BOOST_NOEXCEPT
      {}
#endif
      template <class U>
      BOOST_SYMBOL_VISIBLE
      default_delete(const default_delete<U>&,
          typename enable_if<is_convertible<U*, T*> >::type* = 0) BOOST_NOEXCEPT
      {}
      BOOST_SYMBOL_VISIBLE
      void operator() (T* ptr) const BOOST_NOEXCEPT
      {
        BOOST_STATIC_ASSERT_MSG(sizeof(T) > 0, "default_delete can not delete incomplete type");
        delete ptr;
      }
    };

    template <class T>
    struct BOOST_SYMBOL_VISIBLE default_delete<T[]>
    {
    public:
#ifndef BOOST_NO_CXX11_DEFAULTED_FUNCTIONS
      BOOST_SYMBOL_VISIBLE
      BOOST_CONSTEXPR default_delete() = default;
#else
      BOOST_SYMBOL_VISIBLE
      BOOST_CONSTEXPR default_delete() BOOST_NOEXCEPT
      {}
#endif
      template <class U>
      BOOST_SYMBOL_VISIBLE
      default_delete(const default_delete<U[]>&,
          typename enable_if<detail::same_or_less_cv_qualified<U*, T*> >::type* = 0) BOOST_NOEXCEPT
      {}
      template <class U>
      BOOST_SYMBOL_VISIBLE
      void operator() (U* ptr,
          typename enable_if<detail::same_or_less_cv_qualified<U*, T*> >::type* = 0) const BOOST_NOEXCEPT
      {
        BOOST_STATIC_ASSERT_MSG(sizeof(T) > 0, "default_delete can not delete incomplete type");
        delete [] ptr;
      }
    };
  }
}
#else
namespace boost
{
  namespace csbl
  {
    using ::std::default_delete;
  }
}
#endif // defined  BOOST_NO_CXX11_SMART_PTR

namespace boost
{
  using ::boost::csbl::default_delete;
}
#endif // header
