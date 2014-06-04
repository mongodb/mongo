//////////////////////////////////////////////////////////////////////////////
//
// Copyright (C) 2011-2013 Vicente J. Botet Escriba
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/thread for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_THREAD_DETAIL_MEMORY_HPP
#define BOOST_THREAD_DETAIL_MEMORY_HPP

#include <boost/config.hpp>
#include <boost/container/allocator_traits.hpp>
#include <boost/container/scoped_allocator.hpp>
#include <boost/type_traits/remove_cv.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/is_scalar.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/static_assert.hpp>

namespace boost
{
  namespace thread_detail
  {
    template <class _Alloc>
    class allocator_destructor
    {
      typedef container::allocator_traits<_Alloc> alloc_traits;
    public:
      typedef typename alloc_traits::pointer pointer;
      typedef typename alloc_traits::size_type size_type;
    private:
      _Alloc alloc_;
      size_type s_;
    public:
      allocator_destructor(_Alloc& a, size_type s)BOOST_NOEXCEPT
      : alloc_(a), s_(s)
      {}
      void operator()(pointer p)BOOST_NOEXCEPT
      {
        alloc_traits::destroy(alloc_, p);
        alloc_traits::deallocate(alloc_, p, s_);
      }
    };
  } //namespace thread_detail

  typedef container::allocator_arg_t allocator_arg_t;
  BOOST_CONSTEXPR_OR_CONST allocator_arg_t allocator_arg = {};

  template <class T, class Alloc>
  struct uses_allocator: public container::uses_allocator<T, Alloc>
  {
  };

  template <class Ptr>
  struct pointer_traits
  {
      typedef Ptr pointer;
//      typedef <details> element_type;
//      typedef <details> difference_type;

//      template <class U> using rebind = <details>;
//
//      static pointer pointer_to(<details>);
  };

  template <class T>
  struct pointer_traits<T*>
  {
      typedef T* pointer;
      typedef T element_type;
      typedef ptrdiff_t difference_type;

//      template <class U> using rebind = U*;
//
//      static pointer pointer_to(<details>) noexcept;
  };


  namespace thread_detail {
    template <class _Ptr1, class _Ptr2,
              bool = is_same<typename remove_cv<typename pointer_traits<_Ptr1>::element_type>::type,
                             typename remove_cv<typename pointer_traits<_Ptr2>::element_type>::type
                            >::value
             >
    struct same_or_less_cv_qualified_imp
        : is_convertible<_Ptr1, _Ptr2> {};

    template <class _Ptr1, class _Ptr2>
    struct same_or_less_cv_qualified_imp<_Ptr1, _Ptr2, false>
        : false_type {};

    template <class _Ptr1, class _Ptr2, bool = is_scalar<_Ptr1>::value &&
                                             !is_pointer<_Ptr1>::value>
    struct same_or_less_cv_qualified
        : same_or_less_cv_qualified_imp<_Ptr1, _Ptr2> {};

    template <class _Ptr1, class _Ptr2>
    struct same_or_less_cv_qualified<_Ptr1, _Ptr2, true>
        : false_type {};

  }
  template <class T>
  struct BOOST_SYMBOL_VISIBLE default_delete
  {
  #ifndef BOOST_NO_CXX11_DEFAULTED_FUNCTIONS
    BOOST_SYMBOL_VISIBLE
    BOOST_CONSTEXPR default_delete() = default;
  #else
    BOOST_SYMBOL_VISIBLE
    BOOST_CONSTEXPR default_delete() BOOST_NOEXCEPT {}
  #endif
    template <class U>
    BOOST_SYMBOL_VISIBLE
    default_delete(const default_delete<U>&,
                  typename enable_if<is_convertible<U*, T*> >::type* = 0) BOOST_NOEXCEPT {}
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
    BOOST_CONSTEXPR default_delete() BOOST_NOEXCEPT {}
  #endif
    template <class U>
    BOOST_SYMBOL_VISIBLE
    default_delete(const default_delete<U[]>&,
                   typename enable_if<thread_detail::same_or_less_cv_qualified<U*, T*> >::type* = 0) BOOST_NOEXCEPT {}
    template <class U>
    BOOST_SYMBOL_VISIBLE
    void operator() (U* ptr,
                     typename enable_if<thread_detail::same_or_less_cv_qualified<U*, T*> >::type* = 0) const BOOST_NOEXCEPT
    {
      BOOST_STATIC_ASSERT_MSG(sizeof(T) > 0, "default_delete can not delete incomplete type");
      delete [] ptr;
    }
  };

} // namespace boost


#endif //  BOOST_THREAD_DETAIL_MEMORY_HPP
