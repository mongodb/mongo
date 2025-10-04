//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_CONTAINER_DETAIL_COPY_MOVE_ALGO_HPP
#define BOOST_CONTAINER_DETAIL_COPY_MOVE_ALGO_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

// container
#include <boost/container/allocator_traits.hpp>
// container/detail
#include <boost/container/detail/iterator.hpp>
#include <boost/move/detail/iterator_to_raw_pointer.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/construct_in_place.hpp>
#include <boost/container/detail/destroyers.hpp>

// move
#include <boost/move/adl_move_swap.hpp>
#include <boost/move/iterator.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/move/traits.hpp>
// other
#include <boost/assert.hpp>
// std
#include <cstring> //for memmove/memcpy

#if defined(BOOST_GCC) && (BOOST_GCC >= 40600)
#pragma GCC diagnostic push
//pair memcpy optimizations rightfully detected by GCC
#  if defined(BOOST_GCC) && (BOOST_GCC >= 80000)
#     pragma GCC diagnostic ignored "-Wclass-memaccess"
#  endif
//GCC 8 seems a bit confused about array access error with static_vector
//when out of bound exceptions are being thrown.
#  if defined(BOOST_GCC) && ((BOOST_GCC >= 80000) && (BOOST_GCC < 80200))
#     pragma GCC diagnostic ignored "-Wstringop-overflow"
#  endif
//GCC 12 seems a bit confused about array access error with small_vector
#  if defined(BOOST_GCC) && (BOOST_GCC >= 110000)
#     pragma GCC diagnostic ignored "-Wstringop-overread"
#     pragma GCC diagnostic ignored "-Wstringop-overflow"
#  endif
#  pragma GCC diagnostic ignored "-Warray-bounds"
#endif

namespace boost {
namespace container {
namespace dtl {

template<class I>
struct are_elements_contiguous
{
   BOOST_STATIC_CONSTEXPR bool value = false;
};

/////////////////////////
//    raw pointers
/////////////////////////

template<class T>
struct are_elements_contiguous<T*>
{
   BOOST_STATIC_CONSTEXPR bool value = true;
};

/////////////////////////
//    move iterators
/////////////////////////

template<class It>
struct are_elements_contiguous< ::boost::move_iterator<It> >
   : are_elements_contiguous<It>
{};

}  //namespace dtl {

/////////////////////////
//    predeclarations
/////////////////////////

template <class Pointer, bool IsConst>
class vec_iterator;

}  //namespace container {

namespace interprocess {

template <class PointedType, class DifferenceType, class OffsetType, std::size_t OffsetAlignment>
class offset_ptr;

}  //namespace interprocess {

namespace container {

namespace dtl {

/////////////////////////
//vector_[const_]iterator
/////////////////////////

template <class Pointer, bool IsConst>
struct are_elements_contiguous<boost::container::vec_iterator<Pointer, IsConst> >
{
   BOOST_STATIC_CONSTEXPR bool value = true;
};


/////////////////////////
//    offset_ptr
/////////////////////////

template <class PointedType, class DifferenceType, class OffsetType, std::size_t OffsetAlignment>
struct are_elements_contiguous< ::boost::interprocess::offset_ptr<PointedType, DifferenceType, OffsetType, OffsetAlignment> >
{
   BOOST_STATIC_CONSTEXPR bool value = true;
};

template <typename I, typename O>
struct are_contiguous_and_same
   : boost::move_detail::and_
      < are_elements_contiguous<I>
      , are_elements_contiguous<O>
      , is_same< typename remove_const< typename ::boost::container::iter_value<I>::type >::type
               , typename ::boost::container::iterator_traits<O>::value_type
               >
      >
{};

template <typename I, typename O>
struct is_memtransfer_copy_assignable
   : boost::move_detail::and_
      < are_contiguous_and_same<I, O>
      , dtl::is_trivially_copy_assignable< typename ::boost::container::iter_value<I>::type >
      >
{};

template <typename I, typename O>
struct is_memtransfer_copy_constructible
   : boost::move_detail::and_
      < are_contiguous_and_same<I, O>
      , dtl::is_trivially_copy_constructible< typename ::boost::container::iter_value<I>::type >
      >
{};

template <typename I, typename O, typename R>
struct enable_if_memtransfer_copy_constructible
   : enable_if<dtl::is_memtransfer_copy_constructible<I, O>, R>
{};

template <typename I, typename O, typename R>
struct disable_if_memtransfer_copy_constructible
   : disable_if<dtl::is_memtransfer_copy_constructible<I, O>, R>
{};

template <typename I, typename O, typename R>
struct enable_if_memtransfer_copy_assignable
   : enable_if<dtl::is_memtransfer_copy_assignable<I, O>, R>
{};

template <typename I, typename O, typename R>
struct disable_if_memtransfer_copy_assignable
   : disable_if<dtl::is_memtransfer_copy_assignable<I, O>, R>
{};

template <class T>
struct has_single_value
{
private:
   struct two { char array_[2]; };
   template<bool Arg> struct wrapper;
   template <class U> static two test(int, ...);
   template <class U> static char test(int, const wrapper<U::single_value>*);
public:
   BOOST_STATIC_CONSTEXPR bool value = sizeof(test<T>(0, 0)) == 1;
   void dummy() {}
};

template<class InsertionProxy, bool = has_single_value<InsertionProxy>::value>
struct is_single_value_proxy_impl
{
   BOOST_STATIC_CONSTEXPR bool value = InsertionProxy::single_value;
};

template<class InsertionProxy>
struct is_single_value_proxy_impl<InsertionProxy, false>
{
   BOOST_STATIC_CONSTEXPR bool value = false;
};

template<class InsertionProxy>
struct is_single_value_proxy
   : is_single_value_proxy_impl<InsertionProxy>
{};

template <typename P, typename R = void>
struct enable_if_single_value_proxy
   : enable_if<is_single_value_proxy<P>, R>
{};

template <typename P, typename R = void>
struct disable_if_single_value_proxy
   : disable_if<is_single_value_proxy<P>, R>
{};

template
   <typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline F memmove(I f, I l, F r) BOOST_NOEXCEPT_OR_NOTHROW
{
   typedef typename boost::container::iter_value<I>::type      value_type;
   typedef typename boost::container::iterator_traits<F>::difference_type r_difference_type;
   value_type *const dest_raw = boost::movelib::iterator_to_raw_pointer(r);
   const value_type *const beg_raw = boost::movelib::iterator_to_raw_pointer(f);
   const value_type *const end_raw = boost::movelib::iterator_to_raw_pointer(l);
   if(BOOST_LIKELY(beg_raw != end_raw && dest_raw && beg_raw)){
      const std::size_t n = std::size_t(end_raw - beg_raw)   ;
      std::memmove(dest_raw, beg_raw, sizeof(value_type)*n);
      r += static_cast<r_difference_type>(n);
   }
   return r;
}

template
   <typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline F memmove_n(I f, std::size_t n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{
   typedef typename boost::container::iter_value<I>::type value_type;
   typedef typename boost::container::iterator_traits<F>::difference_type r_difference_type;
   if(BOOST_LIKELY(n != 0)){
      void *dst = boost::movelib::iterator_to_raw_pointer(r);
      const void *src = boost::movelib::iterator_to_raw_pointer(f);
      if (dst && src)
         std::memmove(dst, src, sizeof(value_type)*n);
      r += static_cast<r_difference_type>(n);
   }

   return r;
}

template
   <typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline I memmove_n_source(I f, std::size_t n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{
   if(BOOST_LIKELY(n != 0)){
      typedef typename boost::container::iter_value<I>::type value_type;
      typedef typename boost::container::iterator_traits<I>::difference_type i_difference_type;
      void *dst = boost::movelib::iterator_to_raw_pointer(r);
      const void *src = boost::movelib::iterator_to_raw_pointer(f);
      if (dst && src)
         std::memmove(dst, src, sizeof(value_type)*n);
      f += static_cast<i_difference_type>(n);
   }
   return f;
}

template
   <typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline I memmove_n_source_dest(I f, std::size_t n, F &r) BOOST_NOEXCEPT_OR_NOTHROW
{
   typedef typename boost::container::iter_value<I>::type value_type;
   typedef typename boost::container::iterator_traits<F>::difference_type i_difference_type;
   typedef typename boost::container::iterator_traits<F>::difference_type f_difference_type;

   if(BOOST_LIKELY(n != 0)){
      void *dst = boost::movelib::iterator_to_raw_pointer(r);
      const void *src = boost::movelib::iterator_to_raw_pointer(f);
      if (dst && src)
         std::memmove(dst, src, sizeof(value_type)*n);
      f += i_difference_type(n);
      r += f_difference_type(n);
   }
   return f;
}

template <typename O>
struct is_memzero_initializable
{
   typedef typename ::boost::container::iterator_traits<O>::value_type value_type;
   BOOST_STATIC_CONSTEXPR bool value = are_elements_contiguous<O>::value &&
      (  dtl::is_integral<value_type>::value || dtl::is_enum<value_type>::value
      #if defined(BOOST_CONTAINER_MEMZEROED_POINTER_IS_NULL)
      || dtl::is_pointer<value_type>::value
      #endif
      #if defined(BOOST_CONTAINER_MEMZEROED_FLOATING_POINT_IS_ZERO)
      || dtl::is_floating_point<value_type>::value
      #endif
      );
};

template <typename O, typename R>
struct enable_if_memzero_initializable
   : enable_if_c<dtl::is_memzero_initializable<O>::value, R>
{};

template <typename O, typename R>
struct disable_if_memzero_initializable
   : enable_if_c<!dtl::is_memzero_initializable<O>::value, R>
{};

template <typename I, typename R>
struct enable_if_trivially_destructible
   : enable_if_c < dtl::is_trivially_destructible
                  <typename boost::container::iter_value<I>::type>::value
               , R>
{};

template <typename I, typename R>
struct disable_if_trivially_destructible
   : enable_if_c <!dtl::is_trivially_destructible
                  <typename boost::container::iter_value<I>::type>::value
               , R>
{};

}  //namespace dtl {

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_move_alloc
//
//////////////////////////////////////////////////////////////////////////////


//! <b>Effects</b>:
//!   \code
//!   for (; f != l; ++r, ++f)
//!      allocator_traits::construct(a, &*r, boost::move(*f));
//!   \endcode
//!
//! <b>Returns</b>: r
template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_constructible<I, F, F>::type
   uninitialized_move_alloc(Allocator &a, I f, I l, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (f != l) {
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(r), boost::move(*f));
         ++f; ++r;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return r;
}

template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_constructible<I, F, F>::type
   uninitialized_move_alloc(Allocator &, I f, I l, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove(f, l, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_move_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; n--; ++r, ++f)
//!      allocator_traits::construct(a, &*r, boost::move(*f));
//!   \endcode
//!
//! <b>Returns</b>: r
template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_constructible<I, F, F>::type
   uninitialized_move_alloc_n(Allocator &a, I f, std::size_t n, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (n) {
         --n;
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(r), boost::move(*f));
         ++f; ++r;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return r;
}

template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_constructible<I, F, F>::type
   uninitialized_move_alloc_n(Allocator &, I f, std::size_t n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n(f, n, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_move_alloc_n_source
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; n--; ++r, ++f)
//!      allocator_traits::construct(a, &*r, boost::move(*f));
//!   \endcode
//!
//! <b>Returns</b>: f (after incremented)
template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_constructible<I, F, I>::type
   uninitialized_move_alloc_n_source(Allocator &a, I f, std::size_t n, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (n) {
         --n;
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(r), boost::move(*f));
         ++f; ++r;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return f;
}

template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_constructible<I, F, I>::type
   uninitialized_move_alloc_n_source(Allocator &, I f, std::size_t n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n_source(f, n, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_copy_alloc
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; f != l; ++r, ++f)
//!      allocator_traits::construct(a, &*r, *f);
//!   \endcode
//!
//! <b>Returns</b>: r
template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_constructible<I, F, F>::type
   uninitialized_copy_alloc(Allocator &a, I f, I l, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (f != l) {
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(r), *f);
         ++f; ++r;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return r;
}

template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_constructible<I, F, F>::type
   uninitialized_copy_alloc(Allocator &, I f, I l, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove(f, l, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_copy_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; n--; ++r, ++f)
//!      allocator_traits::construct(a, &*r, *f);
//!   \endcode
//!
//! <b>Returns</b>: r
template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_constructible<I, F, F>::type
   uninitialized_copy_alloc_n(Allocator &a, I f, std::size_t n, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (n) {
         --n;
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(r), *f);
         ++f; ++r;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return r;
}

template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_constructible<I, F, F>::type
   uninitialized_copy_alloc_n(Allocator &, I f, std::size_t n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n(f, n, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_copy_alloc_n_source
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; n--; ++r, ++f)
//!      allocator_traits::construct(a, &*r, *f);
//!   \endcode
//!
//! <b>Returns</b>: f (after incremented)
template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_constructible<I, F, I>::type
   uninitialized_copy_alloc_n_source(Allocator &a, I f, std::size_t n, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (n) {
         boost::container::construct_in_place(a, boost::movelib::iterator_to_raw_pointer(r), f);
         ++f; ++r; --n;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return f;
}

template
   <typename Allocator,
    typename I, // I models InputIterator
    typename F> // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_constructible<I, F, I>::type
   uninitialized_copy_alloc_n_source(Allocator &, I f, std::size_t n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n_source(f, n, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_value_init_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; n--; ++r, ++f)
//!      allocator_traits::construct(a, &*r);
//!   \endcode
//!
//! <b>Returns</b>: r
template
   <typename Allocator,
    typename F> // F models ForwardIterator
inline typename dtl::disable_if_memzero_initializable<F, F>::type
   uninitialized_value_init_alloc_n(Allocator &a, std::size_t n, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (n) {
         --n;
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(r));
         ++r;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return r;
}

template
   <typename Allocator,
    typename F> // F models ForwardIterator
inline typename dtl::enable_if_memzero_initializable<F, F>::type
   uninitialized_value_init_alloc_n(Allocator &, std::size_t n, F r)
{
   typedef typename boost::container::iterator_traits<F>::value_type value_type;
   typedef typename boost::container::iterator_traits<F>::difference_type r_difference_type;

   if (BOOST_LIKELY(n != 0)){
      std::memset((void*)boost::movelib::iterator_to_raw_pointer(r), 0, sizeof(value_type)*n);
      r += static_cast<r_difference_type>(n);
   }
   return r;
}

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_default_init_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; n--; ++r, ++f)
//!      allocator_traits::construct(a, &*r);
//!   \endcode
//!
//! <b>Returns</b>: r
template
   <typename Allocator,
    typename F> // F models ForwardIterator
inline F uninitialized_default_init_alloc_n(Allocator &a, std::size_t n, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (n) {
         --n;
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(r), default_init);
         ++r;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return r;
}

//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_fill_alloc
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; f != l; ++r, ++f)
//!      allocator_traits::construct(a, &*r, *f);
//!   \endcode
//!
//! <b>Returns</b>: r
template
   <typename Allocator,
    typename F, // F models ForwardIterator
    typename T>
inline void uninitialized_fill_alloc(Allocator &a, F f, F l, const T &t)
{
   F back = f;
   BOOST_CONTAINER_TRY{
      while (f != l) {
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(f), t);
         ++f;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != l; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
}


//////////////////////////////////////////////////////////////////////////////
//
//                               uninitialized_fill_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

//! <b>Effects</b>:
//!   \code
//!   for (; n--; ++r, ++f)
//!      allocator_traits::construct(a, &*r, v);
//!   \endcode
//!
//! <b>Returns</b>: r
template
   <typename Allocator,
    typename T,
    typename F> // F models ForwardIterator
inline F uninitialized_fill_alloc_n(Allocator &a, const T &v, std::size_t n, F r)
{
   F back = r;
   BOOST_CONTAINER_TRY{
      while (n) {
         --n;
         allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(r), v);
         ++r;
      }
   }
   BOOST_CONTAINER_CATCH(...){
      for (; back != r; ++back){
         allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(back));
      }
      BOOST_CONTAINER_RETHROW;
   }
   BOOST_CONTAINER_CATCH_END
   return r;
}

//////////////////////////////////////////////////////////////////////////////
//
//                               copy
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I,   // I models InputIterator
typename F>    // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, F>::type
   copy(I f, I l, F r)
{
   while (f != l) {
      *r = *f;
      ++f; ++r;
   }
   return r;
}

template
<typename I,   // I models InputIterator
typename F>    // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, F>::type
   copy(I f, I l, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove(f, l, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                               copy_n
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I,   // I models InputIterator
typename U,   // U models unsigned integral constant
typename F>   // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, F>::type
   copy_n(I f, U n, F r)
{
   while (n) {
      --n;
      *r = *f;
      ++f; ++r;
   }
   return r;
}

template
<typename I,   // I models InputIterator
typename U,   // U models unsigned integral constant
typename F>   // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, F>::type
   copy_n(I f, U n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n(f, n, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                            copy_n_source
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I,   // I models InputIterator
typename U,   // U models unsigned integral constant
typename F>   // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, I>::type
   copy_n_source(I f, U n, F r)
{
   while (n) {
      --n;
      boost::container::assign_in_place(r, f);
      ++f; ++r;
   }
   return f;
}

template
<typename I,   // I models InputIterator
typename F>   // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, I>::type
   copy_n_source(I f, std::size_t n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n_source(f, n, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                            copy_n_source_dest
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I,   // I models InputIterator
typename F>   // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, I>::type
   copy_n_source_dest(I f, std::size_t n, F &r)
{
   while (n) {
      --n;
      *r = *f;
      ++f; ++r;
   }
   return f;
}

template
<typename I,   // I models InputIterator
typename F>   // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, I>::type
   copy_n_source_dest(I f, std::size_t n, F &r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n_source_dest(f, n, r);  }

//////////////////////////////////////////////////////////////////////////////
//
//                         move
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I,   // I models InputIterator
typename F>   // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, F>::type
   move(I f, I l, F r)
{
   while (f != l) {
      *r = ::boost::move(*f);
      ++f; ++r;
   }
   return r;
}

template
<typename I,   // I models InputIterator
typename F>   // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, F>::type
   move(I f, I l, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove(f, l, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                         move_n
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I,   // I models InputIterator
typename U,   // U models unsigned integral constant
typename F>   // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, F>::type
   move_n(I f, U n, F r)
{
   while (n) {
      --n;
      *r = ::boost::move(*f);
      ++f; ++r;
   }
   return r;
}

template
<typename I,   // I models InputIterator
typename U,   // U models unsigned integral constant
typename F>   // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, F>::type
   move_n(I f, U n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n(f, n, r); }


//////////////////////////////////////////////////////////////////////////////
//
//                         move_backward
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I,   // I models BidirectionalIterator
typename F>    // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, F>::type
   move_backward(I f, I l, F r)
{
   while (f != l) {
      --l; --r;
      *r = ::boost::move(*l);
   }
   return r;
}

template
<typename I,   // I models InputIterator
typename F>   // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, F>::type
   move_backward(I f, I l, F r) BOOST_NOEXCEPT_OR_NOTHROW
{
   typedef typename boost::container::iter_value<I>::type value_type;
   const std::size_t n = boost::container::iterator_udistance(f, l);
   if (BOOST_LIKELY(n != 0)){
      r -= n;
      std::memmove((boost::movelib::iterator_to_raw_pointer)(r), (boost::movelib::iterator_to_raw_pointer)(f), sizeof(value_type)*n);
   }
   return r;
}

//////////////////////////////////////////////////////////////////////////////
//
//                         move_n_source_dest
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I    // I models InputIterator
,typename U    // U models unsigned integral constant
,typename F>   // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, I>::type
   move_n_source_dest(I f, U n, F &r)
{
   while (n) {
      --n;
      *r = ::boost::move(*f);
      ++f; ++r;
   }
   return f;
}

template
<typename I    // I models InputIterator
,typename F>   // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, I>::type
   move_n_source_dest(I f, std::size_t n, F &r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n_source_dest(f, n, r); }

//////////////////////////////////////////////////////////////////////////////
//
//                         move_n_source
//
//////////////////////////////////////////////////////////////////////////////

template
<typename I    // I models InputIterator
,typename U    // U models unsigned integral constant
,typename F>   // F models ForwardIterator
inline typename dtl::disable_if_memtransfer_copy_assignable<I, F, I>::type
   move_n_source(I f, U n, F r)
{
   while (n) {
      --n;
      *r = ::boost::move(*f);
      ++f; ++r;
   }
   return f;
}

template
<typename I    // I models InputIterator
,typename F>   // F models ForwardIterator
inline typename dtl::enable_if_memtransfer_copy_assignable<I, F, I>::type
   move_n_source(I f, std::size_t n, F r) BOOST_NOEXCEPT_OR_NOTHROW
{  return dtl::memmove_n_source(f, n, r); }

template<typename F>   // F models ForwardIterator
inline F move_forward_overlapping(F f, F l, F r)
{
   return (f != r) ? (move)(f, l, r) : l;
}

template<typename B>   // B models BidirIterator
inline B move_backward_overlapping(B f, B l, B rl)
{
   return (l != rl) ? (move_backward)(f, l, rl) : f;
}


//////////////////////////////////////////////////////////////////////////////
//
//                               destroy_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

template
   <typename Allocator
   ,typename I   // I models InputIterator
   ,typename U>  // U models unsigned integral constant
inline typename dtl::disable_if_trivially_destructible<I, void>::type
   destroy_alloc_n(Allocator &a, I f, U n)
{
   while(n){
      --n;
      allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(f));
      ++f;
   }
}

template
   <typename Allocator
   ,typename I   // I models InputIterator
   ,typename U>  // U models unsigned integral constant
inline typename dtl::enable_if_trivially_destructible<I, void>::type
   destroy_alloc_n(Allocator &, I, U)
{}

//////////////////////////////////////////////////////////////////////////////
//
//                               destroy_alloc
//
//////////////////////////////////////////////////////////////////////////////

template
   <typename Allocator
   ,typename I>   // I models InputIterator
inline typename dtl::disable_if_trivially_destructible<I, void>::type
   destroy_alloc(Allocator &a, I f, I l)
{
   while(f != l){
      allocator_traits<Allocator>::destroy(a, boost::movelib::iterator_to_raw_pointer(f));
      ++f;
   }
}

template
   <typename Allocator
   ,typename I >  // I models InputIterator
inline typename dtl::enable_if_trivially_destructible<I, void>::type
   destroy_alloc(Allocator &, I, I)
{}

//////////////////////////////////////////////////////////////////////////////
//
//                         deep_swap_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

template
   <std::size_t MaxTmpBytes
   ,typename Allocator
   ,typename F // F models ForwardIterator
   ,typename G // G models ForwardIterator
   >
inline typename dtl::disable_if_memtransfer_copy_assignable<F, G, void>::type
   deep_swap_alloc_n( Allocator &a, F short_range_f, std::size_t  n_i, G large_range_f, std::size_t n_j)
{
   std::size_t n = 0;
   for (; n != n_i ; ++short_range_f, ++large_range_f, ++n){
      boost::adl_move_swap(*short_range_f, *large_range_f);
   }
   boost::container::uninitialized_move_alloc_n(a, large_range_f, std::size_t(n_j - n_i), short_range_f);  // may throw
   boost::container::destroy_alloc_n(a, large_range_f, std::size_t(n_j - n_i));
}

BOOST_CONTAINER_CONSTANT_VAR std::size_t DeepSwapAllocNMaxStorage = std::size_t(1) << std::size_t(11); //2K bytes

template
   <std::size_t MaxTmpBytes
   ,typename Allocator
   ,typename F // F models ForwardIterator
   ,typename G // G models ForwardIterator
   >
inline typename dtl::enable_if_c
   < dtl::is_memtransfer_copy_assignable<F, G>::value && (MaxTmpBytes <= DeepSwapAllocNMaxStorage) && false
   , void>::type
   deep_swap_alloc_n( Allocator &a, F short_range_f, std::size_t n_i, G large_range_f, std::size_t n_j)
{
   typedef typename allocator_traits<Allocator>::value_type value_type;
   typedef typename dtl::aligned_storage
      <MaxTmpBytes, dtl::alignment_of<value_type>::value>::type storage_type;
   storage_type storage;

   const std::size_t n_i_bytes = sizeof(value_type)*n_i;
   void *const large_ptr = static_cast<void*>(boost::movelib::iterator_to_raw_pointer(large_range_f));
   void *const short_ptr = static_cast<void*>(boost::movelib::iterator_to_raw_pointer(short_range_f));
   void *const stora_ptr = static_cast<void*>(boost::movelib::iterator_to_raw_pointer(storage.data));
   std::memcpy(stora_ptr, large_ptr, n_i_bytes);
   std::memcpy(large_ptr, short_ptr, n_i_bytes);
   std::memcpy(short_ptr, stora_ptr, n_i_bytes);
   boost::container::iterator_uadvance(large_range_f, n_i);
   boost::container::iterator_uadvance(short_range_f, n_i);
   boost::container::uninitialized_move_alloc_n(a, large_range_f, std::size_t(n_j - n_i), short_range_f);  // may throw
   boost::container::destroy_alloc_n(a, large_range_f, std::size_t(n_j - n_i));
}

template
   <std::size_t MaxTmpBytes
   ,typename Allocator
   ,typename F // F models ForwardIterator
   ,typename G // G models ForwardIterator
   >
inline typename dtl::enable_if_c
   < dtl::is_memtransfer_copy_assignable<F, G>::value && true//(MaxTmpBytes > DeepSwapAllocNMaxStorage)
   , void>::type
   deep_swap_alloc_n( Allocator &a, F short_range_f, std::size_t n_i, G large_range_f, std::size_t n_j)
{
   typedef typename allocator_traits<Allocator>::value_type value_type;
   typedef typename dtl::aligned_storage
      <DeepSwapAllocNMaxStorage, dtl::alignment_of<value_type>::value>::type storage_type;
   storage_type storage;
   const std::size_t sizeof_storage = sizeof(storage);

   std::size_t n_i_bytes = sizeof(value_type)*n_i;
   char *large_ptr = static_cast<char*>(static_cast<void*>(boost::movelib::iterator_to_raw_pointer(large_range_f)));
   char *short_ptr = static_cast<char*>(static_cast<void*>(boost::movelib::iterator_to_raw_pointer(short_range_f)));
   char *stora_ptr = static_cast<char*>(static_cast<void*>(storage.data));

   std::size_t szt_times = n_i_bytes/sizeof_storage;
   const std::size_t szt_rem = n_i_bytes%sizeof_storage;

   //Loop unrolling using Duff's device, as it seems it helps on some architectures
   const std::size_t Unroll = 4;
   std::size_t n = (szt_times + (Unroll-1))/Unroll;
   const std::size_t branch_number = (szt_times == 0)*Unroll + (szt_times % Unroll);
   switch(branch_number){
      case 4:
         break;
      case 0: do{
         std::memcpy(stora_ptr, large_ptr, sizeof_storage);
         std::memcpy(large_ptr, short_ptr, sizeof_storage);
         std::memcpy(short_ptr, stora_ptr, sizeof_storage);
         large_ptr += sizeof_storage;
         short_ptr += sizeof_storage;
         BOOST_FALLTHROUGH;
      case 3:
         std::memcpy(stora_ptr, large_ptr, sizeof_storage);
         std::memcpy(large_ptr, short_ptr, sizeof_storage);
         std::memcpy(short_ptr, stora_ptr, sizeof_storage);
         large_ptr += sizeof_storage;
         short_ptr += sizeof_storage;
         BOOST_FALLTHROUGH;
      case 2:
         std::memcpy(stora_ptr, large_ptr, sizeof_storage);
         std::memcpy(large_ptr, short_ptr, sizeof_storage);
         std::memcpy(short_ptr, stora_ptr, sizeof_storage);
         large_ptr += sizeof_storage;
         short_ptr += sizeof_storage;
         BOOST_FALLTHROUGH;
      case 1:
         std::memcpy(stora_ptr, large_ptr, sizeof_storage);
         std::memcpy(large_ptr, short_ptr, sizeof_storage);
         std::memcpy(short_ptr, stora_ptr, sizeof_storage);
         large_ptr += sizeof_storage;
         short_ptr += sizeof_storage;
         } while(--n);
   }
   std::memcpy(stora_ptr, large_ptr, szt_rem);
   std::memcpy(large_ptr, short_ptr, szt_rem);
   std::memcpy(short_ptr, stora_ptr, szt_rem);
   boost::container::iterator_uadvance(large_range_f, n_i);
   boost::container::iterator_uadvance(short_range_f, n_i);
   boost::container::uninitialized_move_alloc_n(a, large_range_f, std::size_t(n_j - n_i), short_range_f);  // may throw
   boost::container::destroy_alloc_n(a, large_range_f, std::size_t(n_j - n_i));
}


//////////////////////////////////////////////////////////////////////////////
//
//                         copy_assign_range_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

template
   <typename Allocator
   ,typename I // F models InputIterator
   ,typename O // G models OutputIterator
   >
void copy_assign_range_alloc_n( Allocator &a, I inp_start, std::size_t n_i, O out_start, std::size_t n_o )
{
   if (n_o < n_i){
      inp_start = boost::container::copy_n_source_dest(inp_start, n_o, out_start);     // may throw
      boost::container::uninitialized_copy_alloc_n(a, inp_start, std::size_t(n_i - n_o), out_start);// may throw
   }
   else{
      out_start = boost::container::copy_n(inp_start, n_i, out_start);  // may throw
      boost::container::destroy_alloc_n(a, out_start, std::size_t(n_o - n_i));
   }
}

//////////////////////////////////////////////////////////////////////////////
//
//                         move_assign_range_alloc_n
//
//////////////////////////////////////////////////////////////////////////////

template
   <typename Allocator
   ,typename I // F models InputIterator
   ,typename O // G models OutputIterator
   >
void move_assign_range_alloc_n( Allocator &a, I inp_start, std::size_t n_i, O out_start, std::size_t n_o )
{
   if (n_o < n_i){
      inp_start = boost::container::move_n_source_dest(inp_start, n_o, out_start);  // may throw
      boost::container::uninitialized_move_alloc_n(a, inp_start, std::size_t(n_i - n_o), out_start);  // may throw
   }
   else{
      out_start = boost::container::move_n(inp_start, n_i, out_start);  // may throw
      boost::container::destroy_alloc_n(a, out_start, std::size_t(n_o - n_i));
   }
}

template<class Allocator>
struct array_destructor
{
   typedef typename ::boost::container::allocator_traits<Allocator>::value_type value_type;
   typedef typename dtl::if_c
      <dtl::is_trivially_destructible<value_type>::value
      ,dtl::null_scoped_destructor_range<Allocator>
      ,dtl::scoped_destructor_range<Allocator>
      >::type type;
};

template<class Allocator>
struct value_destructor
{
   typedef typename ::boost::container::allocator_traits<Allocator>::value_type value_type;
   typedef typename dtl::if_c
      <dtl::is_trivially_destructible<value_type>::value
      , dtl::null_scoped_destructor<Allocator>
      , dtl::scoped_destructor<Allocator>
      >::type type;
};

template
   <typename Allocator
   ,typename F // F models ForwardIterator
   ,typename O // G models OutputIterator
   ,typename InsertionProxy
   >
void uninitialized_move_and_insert_alloc
   ( Allocator &a
   , F first
   , F pos
   , F last
   , O d_first
   , std::size_t n
   , InsertionProxy insertion_proxy)
{
   typedef typename array_destructor<Allocator>::type array_destructor_t;

   //Anti-exception rollbacks
   array_destructor_t new_values_destroyer(d_first, d_first, a);

   //Initialize with [begin(), pos) old buffer
   //the start of the new buffer
   O d_last = ::boost::container::uninitialized_move_alloc(a, first, pos, d_first);
   new_values_destroyer.set_end(d_last);
   //Initialize new objects, starting from previous point
   insertion_proxy.uninitialized_copy_n_and_update(a, d_last, n);
   d_last += n;
   new_values_destroyer.set_end(d_last);
   //Initialize from the rest of the old buffer,
   //starting from previous point
   (void) ::boost::container::uninitialized_move_alloc(a, pos, last, d_last);
   //All construction successful, disable rollbacks
   new_values_destroyer.release();
}




template
   <typename Allocator
   ,typename F // F models ForwardIterator
   ,typename InsertionProxy
   >
typename dtl::enable_if_c<dtl::is_single_value_proxy<InsertionProxy>::value, void>::type
   expand_backward_and_insert_nonempty_middle_alloc
   ( Allocator &a
   , F const first
   , F const pos
   , std::size_t const
   , InsertionProxy insertion_proxy)
{
   BOOST_ASSERT(first != pos);
 
   typedef typename value_destructor<Allocator>::type value_destructor_t;
   F aux = first;   --aux;
   allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(aux), boost::move(*first));
   value_destructor_t on_exception(a, boost::movelib::iterator_to_raw_pointer(aux));
   //Copy previous to last objects to the initialized end
   aux = first; ++aux;
   aux = boost::container::move(aux, pos, first);
   //Insert new objects in the pos
   insertion_proxy.copy_n_and_update(a, aux, 1u);
   on_exception.release();
}

template
   <typename Allocator
   ,typename F // F models ForwardIterator
   ,typename InsertionProxy
   >
typename dtl::disable_if_c<dtl::is_single_value_proxy<InsertionProxy>::value, void>::type
   expand_backward_and_insert_nonempty_middle_alloc
   ( Allocator &a
   , F first
   , F pos
   , std::size_t const n
   , InsertionProxy insertion_proxy)
{
   BOOST_ASSERT(first != pos);
   BOOST_ASSERT(n != 0);

   typedef typename array_destructor<Allocator>::type array_destructor_t;
   const std::size_t elems_before = iterator_udistance(first, pos);
   if(elems_before >= n){
      //New elements can be just copied.
      //Move to uninitialized memory last objects
      F const first_less_n = first - n;
      F nxt = ::boost::container::uninitialized_move_alloc_n_source(a, first, n, first_less_n);
      array_destructor_t on_exception(first_less_n, first, a);
      //Copy previous to last objects to the initialized end
      nxt = boost::container::move(nxt, pos, first);
      //Insert new objects in the pos
      insertion_proxy.copy_n_and_update(a, nxt, n);
      on_exception.release();
   }
   else {
      //The new elements don't fit in the [pos, end()) range.
      //Copy old [pos, end()) elements to the uninitialized memory (a gap is created)
      F aux = ::boost::container::uninitialized_move_alloc(a, first, pos, first - n);
      array_destructor_t on_exception(first -n, aux, a);
      //Copy to the beginning of the unallocated zone the last new elements (the gap is closed).
      insertion_proxy.uninitialized_copy_n_and_update(a, aux, std::size_t(n - elems_before));
      insertion_proxy.copy_n_and_update(a, first, elems_before);
      on_exception.release();
   }
}


template
   <typename Allocator
   ,typename F // F models ForwardIterator
   ,typename InsertionProxy
   >
typename dtl::enable_if_c<dtl::is_single_value_proxy<InsertionProxy>::value, void>::type
   expand_forward_and_insert_nonempty_middle_alloc
   ( Allocator &a
   , F pos
   , F last
   , std::size_t const
   , InsertionProxy insertion_proxy)
{
   BOOST_ASSERT(last != pos);
 
   typedef typename value_destructor<Allocator>::type value_destructor_t;
   F last_m_n = last;   --last_m_n;
   allocator_traits<Allocator>::construct(a, boost::movelib::iterator_to_raw_pointer(last), boost::move(*last_m_n));
   value_destructor_t on_exception(a, boost::movelib::iterator_to_raw_pointer(last));
   //Copy previous to last objects to the initialized end
   boost::container::move_backward(pos, last_m_n, last);
   //Insert new objects in the pos
   insertion_proxy.copy_n_and_update(a, pos, 1);
   on_exception.release();
}

template
   <typename Allocator
   ,typename F // F models ForwardIterator
   ,typename InsertionProxy
   >
typename dtl::disable_if_c<dtl::is_single_value_proxy<InsertionProxy>::value, void>::type
   expand_forward_and_insert_nonempty_middle_alloc
   ( Allocator &a
   , F pos
   , F last
   , std::size_t const n
   , InsertionProxy insertion_proxy)
{
   BOOST_ASSERT(last != pos);
   BOOST_ASSERT(n != 0);

   typedef typename array_destructor<Allocator>::type array_destructor_t;
   const std::size_t elems_after = iterator_udistance(pos, last);
   if(elems_after >= n){
      //New elements can be just copied.
      //Move to uninitialized memory last objects
      F const last_m_n = last - n;
      F const nxt = ::boost::container::uninitialized_move_alloc_n(a, last_m_n, n, last);
      array_destructor_t on_exception(last, nxt, a);
      //Copy previous to last objects to the initialized end
      boost::container::move_backward(pos, last_m_n, last);
      //Insert new objects in the pos
      insertion_proxy.copy_n_and_update(a, pos, n);
      on_exception.release();
   }
   else {
      //The new elements don't fit in the [pos, end()) range.
      //Copy old [pos, end()) elements to the uninitialized memory (a gap is created)
      F new_last = ::boost::container::uninitialized_move_alloc(a, pos, last, pos + n);
      array_destructor_t on_exception(pos + n, new_last, a);
      //Copy first new elements in pos (gap is still there)
      insertion_proxy.copy_n_and_update(a, pos, elems_after);
      //Copy to the beginning of the unallocated zone the last new elements (the gap is closed).
      insertion_proxy.uninitialized_copy_n_and_update(a, last, std::size_t(n - elems_after));
      on_exception.release();
   }
}

template
<typename Allocator
   , typename F // F models ForwardIterator
   , typename InsertionProxy
>
inline void expand_forward_and_insert_alloc
   ( Allocator& a
   , F pos
   , F last
   , std::size_t const n
   , InsertionProxy insertion_proxy)
{
   if (last == pos) {
      insertion_proxy.uninitialized_copy_n_and_update(a, last, n);
   }
   else{
      const bool single_value = dtl::is_single_value_proxy<InsertionProxy>::value;
      BOOST_IF_CONSTEXPR(!single_value){
         if (BOOST_UNLIKELY(!n)) {
            return;
         }
      }
      expand_forward_and_insert_nonempty_middle_alloc(a, pos, last, n, insertion_proxy);
   }
}

template <class B, class InsertionProxy, class Allocator>
void expand_backward_forward_and_insert_alloc_move_backward
( B const old_start
, std::size_t const old_size
, B const new_start
, B const pos
, std::size_t const n
, InsertionProxy insertion_proxy
, Allocator& a)
{
   typedef std::size_t size_type;
   typedef typename allocator_traits<Allocator>::value_type value_type;
   BOOST_STATIC_CONSTEXPR bool trivial_dctr_after_move = has_trivial_destructor_after_move<value_type>::value;
   BOOST_STATIC_CONSTEXPR bool trivial_dctr = dtl::is_trivially_destructible<value_type>::value;

   typedef typename dtl::if_c
      <trivial_dctr
      , dtl::null_scoped_destructor_n<Allocator, B>
      , dtl::scoped_destructor_n<Allocator, B>
      >::type   array_destructor_t;

   //n can be zero to just expand capacity
   B old_finish = make_iterator_uadvance(old_start, old_size);

   //We can have 8 possibilities:
   const size_type elemsbefore = static_cast<size_type>(iterator_udistance(old_start, pos));
   const size_type raw_before  = static_cast<size_type>(iterator_udistance(new_start, old_start));
   const size_type before_plus_new = size_type(elemsbefore + n);

   //Check if raw_before is big enough to hold the beginning of old data + new data
   if (raw_before >= before_plus_new) {
      //If anything goes wrong, this object will destroy
      //all the old objects to fulfill previous vector state
      array_destructor_t old_values_destroyer(old_start, a, old_size);
      // _________________________________________________________
      //|            raw_mem                | old_begin | old_end |  //Old situation
      //| __________________________________|___________|_________|
      // _________________________________________________________
      //| old_begin |    new   |  raw_mem   | old_begin | old_end |  //First step
      //|___________|__________|____________|___________|_________|

      //Copy first old values before pos, after that the new objects
      B const new_elem_pos = ::boost::container::uninitialized_move_alloc(a, old_start, pos, new_start);
      array_destructor_t new_values_destroyer(new_start, a, elemsbefore);
      insertion_proxy.uninitialized_copy_n_and_update(a, new_elem_pos, n);
      new_values_destroyer.set_size(before_plus_new);
      const size_type new_size = size_type(old_size + n);
      //Check if raw_before is so big that even copying the old data + new data
      //there is a gap between the new data and the old data
      if (raw_before >= new_size) {
         // _______________________________________________________
         //|            raw_mem              | old_begin | old_end | //Old situation
         //|_________________________________|___________|_________|
         // _______________________________________________________
         //| old_begin |   new  |  raw_mem   | old_begin | old_end | //First step
         //|___________|________|____________|___________|_________|
         // _______________________________________________________
         //| old_begin |   new  | old_end |       raw_mem          | //New situation
         //|___________|________|_________|________________________|
         //
         //Now initialize the rest of memory with the last old values
         if (before_plus_new != new_size) { //Special case to avoid operations in back insertion
            B new_start_end(make_iterator_uadvance(new_start, before_plus_new));
            ::boost::container::uninitialized_move_alloc(a, pos, old_finish, new_start_end);
         }
         //All new elements correctly constructed, avoid new element destruction
         new_values_destroyer.release();
         //Old values destroyed automatically with "old_values_destroyer"
         //when "old_values_destroyer" goes out of scope unless the have trivial
         //destructor after move.
         if(trivial_dctr_after_move)
            old_values_destroyer.release();
      }
      //raw_before is so big that divides old_end
      else {
         // _________________________________________________
         //|               raw           | old_beg | old_end | //Old situation
         //|_____________________________|_________|_________|
         // _________________________________________________
         //| old_begin |    new   |  raw | old_beg | old_end | //First step
         //|___________|__________|______|_________|_________|
         // _________________________________________________
         //| old_begin |    new   | old_end |  raw_mem       | //New situation
         //|___________|__________|_________|________________|

         //Now initialize the rest of memory with the last old values
         //All new elements correctly constructed, avoid new element destruction
         BOOST_IF_CONSTEXPR(!trivial_dctr) {
            //Now initialize the rest of raw_before memory with the
            //first of elements after new values
            const size_type raw_gap = raw_before - before_plus_new;
            B new_start_plus(make_iterator_uadvance(new_start, before_plus_new));
            ::boost::container::uninitialized_move_alloc_n(a, pos, raw_gap, new_start_plus);
            new_values_destroyer.release();
            old_values_destroyer.increment_size_backwards(raw_before);
            //Now move remaining last objects in the old buffer begin
            B remaining_pos(make_iterator_uadvance(pos, raw_gap));
            remaining_pos = ::boost::container::move_forward_overlapping(remaining_pos, old_finish, old_start);
            (void)remaining_pos;
            //Once moved, avoid calling the destructors if trivial after move
            if(!trivial_dctr_after_move) {
               boost::container::destroy_alloc(a, remaining_pos, old_finish);
            }
         }
         else { //If trivial destructor, we can uninitialized copy + copy in a single uninitialized copy
            ::boost::container::uninitialized_move_alloc_n
               (a, pos, static_cast<size_type>(old_finish - pos), make_iterator_uadvance(new_start, before_plus_new));
         }
         old_values_destroyer.release();
      }
   }
   else {
      //If anything goes wrong, this object will destroy
      //all the old objects to fulfill previous vector state
      array_destructor_t old_values_destroyer(old_start, a, old_size);

      //Check if we have to do the insertion in two phases
      //since maybe raw_before is not big enough and
      //the buffer was expanded both sides
      // _________________________________________________
      //| raw_mem | old_begin + old_end |  raw_mem        | //Old situation
      //|_________|_____________________|_________________|
      // _________________________________________________
      //|     old_begin + new + old_end     |  raw_mem    | //New situation with do_after
      //|___________________________________|_____________|
      // _________________________________________________
      //| old_begin + new + old_end  |  raw_mem           | //New without do_after
      //|____________________________|____________________|
      //
      const bool do_after = n > raw_before;

      //Now we can have two situations: the raw_mem of the
      //beginning divides the old_begin, or the new elements:
      if (raw_before <= elemsbefore) {
         //The raw memory divides the old_begin group:
         //
         //If we need two phase construction (do_after)
         //new group is divided in new = new_beg + new_end groups
         //In this phase only new_beg will be inserted
         //
         // _________________________________________________
         //| raw_mem | old_begin | old_end |  raw_mem        | //Old situation
         //|_________|___________|_________|_________________|
         // _________________________________________________
         //| old_begin | new_beg | old_end |  raw_mem        | //New situation with do_after(1),
         //|___________|_________|_________|_________________| //not definitive, pending operations
         // _________________________________________________
         //| old_begin | new | old_end |  raw_mem            | //New situation without do_after,
         //|___________|_____|_________|_____________________| //definitive.
         //
         //Copy the first part of old_begin to raw_mem
         ::boost::container::uninitialized_move_alloc_n(a, old_start, raw_before, new_start);
         //The buffer is all constructed until old_end,
         //so program trailing destruction and assign final size
         //if !do_after, raw_before+n otherwise.
         size_type new_1st_range;
         old_values_destroyer.increment_size_backwards(raw_before);
         new_1st_range = do_after ? raw_before : n;

         //Now copy the second part of old_begin overwriting itself
         B const old_next(make_iterator_uadvance(old_start, raw_before));
         B const next = ::boost::container::move(old_next, pos, old_start);
         //Now copy the new_beg elements
         insertion_proxy.copy_n_and_update(a, next, new_1st_range);

         //If there is no after work and the last old part needs to be moved to front, do it
         if (!do_after) {
            //Now displace old_end elements and destroy trailing
            B const new_first(make_iterator_uadvance(next, new_1st_range));
            B const p = ::boost::container::move_forward_overlapping(pos, old_finish, new_first);
            (void)p;
            if(!trivial_dctr_after_move)
               boost::container::destroy_alloc(a, p, old_finish);
         }
      }
      else {
         //If we have to expand both sides,
         //we will play if the first new values so
         //calculate the upper bound of new values

         //The raw memory divides the new elements
         //
         //If we need two phase construction (do_after)
         //new group is divided in new = new_beg + new_end groups
         //In this phase only new_beg will be inserted
         //
         // ____________________________________________________
         //|   raw_mem     | old_begin | old_end |  raw_mem     | //Old situation
         //|_______________|___________|_________|______________|
         // ____________________________________________________
         //| old_begin |    new_beg    | old_end |  raw_mem     | //New situation with do_after(),
         //|___________|_______________|_________|______________| //not definitive, pending operations
         // ____________________________________________________
         //| old_begin | new | old_end |  raw_mem               | //New situation without do_after,
         //|___________|_____|_________|________________________| //definitive
         //
         //First copy whole old_begin and part of new to raw_mem
         B const new_pos = ::boost::container::uninitialized_move_alloc(a, old_start, pos, new_start);
         array_destructor_t new_values_destroyer(new_start, a, elemsbefore);
         const size_type mid_n = size_type(raw_before - elemsbefore);
         insertion_proxy.uninitialized_copy_n_and_update(a, new_pos, mid_n);
         new_values_destroyer.release();
         //The buffer is all constructed until old_end
         old_values_destroyer.increment_size_backwards(raw_before);

         if (do_after) {
            //Copy new_beg part
            insertion_proxy.copy_n_and_update(a, old_start, elemsbefore);
         }
         else {
            //Copy all new elements
            const size_type rest_new = size_type(n - mid_n);
            insertion_proxy.copy_n_and_update(a, old_start, rest_new);

            B move_start(make_iterator_uadvance(old_start, rest_new));

            //Displace old_end, but make sure data has to be moved
            B const move_end = ::boost::container::move_forward_overlapping(pos, old_finish, move_start);
            (void)move_end;   //To avoid warnings of unused initialization for move_end in case
                              //trivial_dctr_after_move is true
            //Destroy remaining moved elements from old_end except if they
            //have trivial destructor after being moved
            if(!trivial_dctr_after_move) {
               boost::container::destroy_alloc(a, move_end, old_finish);
            }
         }
      }

      //This is only executed if two phase construction is needed
      if (do_after) {
         //The raw memory divides the new elements
         // ______________________________________________________
         //|   raw_mem    | old_begin |  old_end   |  raw_mem     |  //Old situation
         //|______________|___________|____________|______________|
         // _______________________________________________________
         //| old_begin   +   new_beg  | new_end |old_end | rawmem |  //New situation with do_after(1)
         //|__________________________|_________|________|________|
         // ______________________________________________________
         //| old_begin      +       new            | old_end |raw |  //New situation with do_after(2)
         //|_______________________________________|_________|____|
         const size_type n_after = size_type(n - raw_before);
         const size_type elemsafter = size_type(old_size - elemsbefore);

         //We can have two situations:
         if (elemsafter >= n_after) {
            //The raw_mem from end will divide displaced old_end
            //
            //Old situation:
            // ______________________________________________________
            //|   raw_mem    | old_begin |  old_end   |  raw_mem     |
            //|______________|___________|____________|______________|
            //
            //New situation with do_after(1):
            // _______________________________________________________
            //| old_begin   +   new_beg  | new_end |old_end | raw_mem |
            //|__________________________|_________|________|_________|
            //
            //First copy the part of old_end raw_mem
            B finish_n = make_iterator_advance(old_finish, -std::ptrdiff_t(n_after));
            ::boost::container::uninitialized_move_alloc(a, finish_n, old_finish, old_finish);
            old_values_destroyer.increment_size(n_after);
            //Displace the rest of old_end to the new position
            boost::container::move_backward_overlapping(pos, finish_n, old_finish);
            //Now overwrite with new_end
            //The new_end part is [first + (n - n_after), last)
            insertion_proxy.copy_n_and_update(a, pos, n_after);
         }
         else {
            //The raw_mem from end will divide new_end part
            // _____________________________________________________________
            //|   raw_mem    | old_begin |  old_end   |  raw_mem            | //Old situation
            //|______________|___________|____________|_____________________|
            // _____________________________________________________________
            //| old_begin   +   new_beg  |     new_end   |old_end | raw_mem | //New situation with do_after(2)
            //|__________________________|_______________|________|_________|

            //First initialize data in raw memory
            const size_type mid_last_dist = size_type(n_after - elemsafter);

            //Copy to the old_end part to the uninitialized zone leaving a gap.
            B const mid_last(make_iterator_uadvance(old_finish, mid_last_dist));
            ::boost::container::uninitialized_move_alloc(a, pos, old_finish, mid_last);

            array_destructor_t old_end_destroyer(mid_last, a, iterator_udistance(pos, old_finish));

            //Copy the first part to the already constructed old_end zone
            insertion_proxy.copy_n_and_update(a, pos, elemsafter);
            //Copy the rest to the uninitialized zone filling the gap
            insertion_proxy.uninitialized_copy_n_and_update(a, old_finish, mid_last_dist);
            old_end_destroyer.release();
         }
      }
      old_values_destroyer.release();
   }
}

template
<typename Allocator
   , typename B // B models BidirIterator
   , typename InsertionProxy
>
inline void expand_backward_forward_and_insert_alloc_move_forward
   ( B const old_start
   , std::size_t const old_size
   , B const new_start
   , B const pos
   , std::size_t const n
   , InsertionProxy insertion_proxy
   , Allocator& a)
{
   typedef std::size_t size_type;
   typedef typename allocator_traits<Allocator>::value_type value_type;
   BOOST_STATIC_CONSTEXPR bool trivial_dctr_after_move = has_trivial_destructor_after_move<value_type>::value;
   BOOST_STATIC_CONSTEXPR bool trivial_dctr = dtl::is_trivially_destructible<value_type>::value;

   typedef typename dtl::if_c
      <trivial_dctr
      , dtl::null_scoped_destructor_n<Allocator, B>
      , dtl::scoped_destructor_n<Allocator, B>
      >::type   array_destructor_t;

   //n can be zero to just expand capacity

   B const old_finish = make_iterator_uadvance(old_start, old_size);
   const size_type new_size = size_type(old_size + n);
   B const new_finish = make_iterator_uadvance(new_start, new_size);

   //We can have 8 possibilities:

   const size_type elemsafter = static_cast<size_type>(iterator_udistance(pos, old_finish));
   const size_type raw_after = static_cast<size_type>(iterator_udistance(old_finish, new_finish));

   const size_type after_plus_new = size_type(elemsafter + n);

   //Check if raw_before is big enough to hold the new data + the end of old data
   if (raw_after >= after_plus_new) {
      //If anything goes wrong, this object will destroy
      //all the old objects to fulfill previous vector state
      array_destructor_t old_values_destroyer(old_start, a, old_size);
      //______________________ __________________________________
      //| old_begin | old_end |            raw_mem                  //Old situation
      //|___________|_________|__________________________________
      // _____________________ _________________________________
      //| old_begin | old_end |  raw_mem |    new   |  old_end  |  //First step
      //|___________|_________|__________|__________|___________|

      //Copy first new objects, after that old values after pos
      B new_elem_pos = new_finish - after_plus_new;
      insertion_proxy.uninitialized_copy_n_and_update(a, new_elem_pos, n);
      array_destructor_t new_values_destroyer(new_elem_pos, a, n);
      ::boost::container::uninitialized_move_alloc(a, pos, old_finish, new_elem_pos+n);
      new_values_destroyer.set_size(after_plus_new);

      //Check if raw_before is so big that even copying the old data + new data
      //there is a gap between the new data and the old data
      if (raw_after >= new_size) {
         //______________________ __________________________________
         //| old_begin | old_end |            raw_mem                  //Old situation
         //|___________|_________|__________________________________
         // _____________________ _________________________________
         //| old_begin | old_end |    raw_mem   |   new  | old_end |  //First step
         //|___________|_________|______________|________|_________|
         // _____________________V_________________________________
         //|       raw_mem          | old_begin |   new  | old_end | //New situation
         //|________________________|___________|________|_________|
         //
         //Now initialize the rest of memory with the last old values
         ::boost::container::uninitialized_move_alloc(a, old_start, pos, new_start);
         //All new elements correctly constructed, avoid new element destruction
         new_values_destroyer.release();
         //Old values destroyed automatically with "old_values_destroyer"
         //when "old_values_destroyer" goes out of scope unless the have trivial
         //destructor after move.
         if(trivial_dctr_after_move)
            old_values_destroyer.release();
      }
      //raw_before is so big that divides old_end
      else {
         //______________________ ____________________________
         //| old_begin | old_end |          raw_mem             //Old situation
         //|___________|_________|____________________________
         // _____________________ ____________________________
         //| old_begin | old_end | raw_mem |   new  | old_end | //First step
         //|___________|_________|_________|________|_________|
         // _________________________________________________
         //|       raw_mem     | old_begin |   new  | old_end | //New situation
         //|___________________|___________|________|_________|

         //Now initialize the rest of raw_before memory with the
         //last elements before new values
         const size_type raw_gap = raw_after - after_plus_new;
         B const pre_pos_raw = pos - raw_gap;
         ::boost::container::uninitialized_move_alloc_n(a, pre_pos_raw, raw_gap, old_finish);
         new_values_destroyer.release();
         old_values_destroyer.increment_size(raw_after);
         //Now move remaining last objects in the old buffer begin
         BOOST_ASSERT(old_start != old_finish);
         boost::container::move_backward_overlapping(old_start, pre_pos_raw, old_finish);
         old_values_destroyer.release();
         if (!trivial_dctr_after_move) {
            boost::container::destroy_alloc(a, old_start, new_start);
         }
      }
   }
   else{
      //If anything goes wrong, this object will destroy
      //all the old objects to fulfill previous vector state
      array_destructor_t old_values_destroyer(old_start, a, old_size);

      //Now we can have two situations: the raw_mem of the
      //end divides the new elements or the old_end
      if (raw_after > elemsafter) {
         //The raw memory divides the new elements
         //__________________________________
         //| old_begin | old_end |    raw    |  //Old situation
         //|___________|_________|___________|
         // _____ ___________________________
         //| raw | old_begin | new | old_end |  //New situation
         //|_____|___________|_____|_________|

         //First copy whole old_end and part of new to raw_mem
         B p = new_finish - elemsafter;
         ::boost::container::uninitialized_move_alloc(a, pos, old_finish, p);
         array_destructor_t new_values_destroyer(p, a, elemsafter);
         //Copy all new elements
         const size_type mid_n = size_type(raw_after - elemsafter);
         const size_type rest_new = size_type(n - mid_n);
         B new_rng_start = old_finish - rest_new;
         insertion_proxy.copy_n_and_update(a, new_rng_start, rest_new);
         insertion_proxy.uninitialized_copy_n_and_update(a, old_finish, mid_n);
         new_values_destroyer.release();
         old_values_destroyer.increment_size_backwards(raw_after);
         //Displace old_end, but make sure data has to be moved
         p = ::boost::container::move_backward_overlapping(old_start, pos, new_rng_start);

         //Destroy remaining moved elements from old_begin except if they
         //have trivial destructor after being moved
         old_values_destroyer.release();
         if (!trivial_dctr_after_move) {
            boost::container::destroy_alloc(a, old_start, p);
         }
      }
      else {
         //The raw memory divides the old_end group:
         //________________________________________
         //| old_begin |    old_end    |      raw  |  //Old situation
         //|___________|_______________|___________|
         // _____ __________________________________
         //| raw | old_begin | new |    old_end    |  //New situation
         //|_____|___________|_____|_______________|
         //
         //Copy the last part of old_end to raw_mem
         const B old_end_pivot = old_finish - raw_after;
         ::boost::container::uninitialized_move_alloc_n(a, old_end_pivot, raw_after, old_finish);
         //The buffer is all constructed
         old_values_destroyer.increment_size_backwards(raw_after);

         //Now copy the first part of old_end overwriting itself
         B const new_end_pos = ::boost::container::move_backward_overlapping(pos, old_end_pivot, old_finish);
         B const new_beg_pos = new_end_pos - n;

         //Now copy the new_beg elements
         insertion_proxy.copy_n_and_update(a, new_beg_pos, n);
         B const p = ::boost::container::move_backward_overlapping(old_start, pos, new_beg_pos);
         old_values_destroyer.release();

         if (!trivial_dctr_after_move) {
            (void)p;
            boost::container::destroy_alloc(a, old_start, p);
         }
      }
   }
}

template <class R, class InsertionProxy, class Allocator>
void expand_backward_forward_and_insert_alloc
   ( R const old_start
   , std::size_t const old_size
   , R const new_start
   , R const pos
   , std::size_t const n
   , InsertionProxy insertion_proxy
   , Allocator& a)
{
   if(new_start < old_start){
      expand_backward_forward_and_insert_alloc_move_backward(old_start, old_size, new_start, pos, n, insertion_proxy, a);
   }
   else{
      expand_backward_forward_and_insert_alloc_move_forward(old_start, old_size, new_start, pos, n, insertion_proxy, a);
   }
}

}  //namespace container {
}  //namespace boost {

//#pragma GCC diagnostic ignored "-Wclass-memaccess"
#if defined(BOOST_GCC) && (BOOST_GCC >= 40600)
#pragma GCC diagnostic pop
#endif

#endif   //#ifndef BOOST_CONTAINER_DETAIL_COPY_MOVE_ALGO_HPP
