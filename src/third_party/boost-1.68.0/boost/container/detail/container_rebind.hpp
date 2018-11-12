//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2017-2017. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_CONTAINER_DETAIL_CONTAINER_REBIND_HPP
#define BOOST_CONTAINER_DETAIL_CONTAINER_REBIND_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/container/allocator_traits.hpp>


namespace boost {
namespace container {
namespace dtl {

   template <class Cont, class U>
   struct container_rebind;

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   template <template <class, class, class...> class Cont, typename V, typename A, class... An, class U>
   struct container_rebind<Cont<V, A, An...>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, An...> type;
   };

   //Needed for non-conforming compilers like GCC 4.3
   template <template <class, class> class Cont, typename V, typename A, class U>
   struct container_rebind<Cont<V, A>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type> type;
   };

   template <template <class> class Cont, typename V, class U>
   struct container_rebind<Cont<V>, U>
   {
      typedef Cont<U> type;
   };

   //for small_vector,static_vector

   template <template <class, std::size_t, class, class...> class Cont, typename V, std::size_t N, typename A, class... An, class U>
   struct container_rebind<Cont<V, N, A, An...>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, An...> type;
   };

   //Needed for non-conforming compilers like GCC 4.3
   template <template <class, std::size_t, class> class Cont, typename V, std::size_t N, typename A, class U>
   struct container_rebind<Cont<V, N, A>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type> type;
   };

   template <template <class, std::size_t> class Cont, typename V, std::size_t N, class U>
   struct container_rebind<Cont<V, N>, U>
   {
      typedef Cont<U, N> type;
   };

#else //C++03 compilers

   template <template <class> class Cont  //0arg
      , typename V
      , class U>
      struct container_rebind<Cont<V>, U>
   {
      typedef Cont<U> type;
   };

   template <template <class, class> class Cont  //0arg
      , typename V, typename A
      , class U>
      struct container_rebind<Cont<V, A>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type> type;
   };

   template <template <class, class, class> class Cont  //1arg
      , typename V, typename A, class P0
      , class U>
      struct container_rebind<Cont<V, A, P0>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0> type;
   };

   template <template <class, class, class, class> class Cont  //2arg
      , typename V, typename A, class P0, class P1
      , class U>
      struct container_rebind<Cont<V, A, P0, P1>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1> type;
   };

   template <template <class, class, class, class, class> class Cont  //3arg
      , typename V, typename A, class P0, class P1, class P2
      , class U>
      struct container_rebind<Cont<V, A, P0, P1, P2>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2> type;
   };

   template <template <class, class, class, class, class, class> class Cont  //4arg
      , typename V, typename A, class P0, class P1, class P2, class P3
      , class U>
      struct container_rebind<Cont<V, A, P0, P1, P2, P3>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3> type;
   };

   template <template <class, class, class, class, class, class, class> class Cont  //5arg
      , typename V, typename A, class P0, class P1, class P2, class P3, class P4
      , class U>
      struct container_rebind<Cont<V, A, P0, P1, P2, P3, P4>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4> type;
   };

   template <template <class, class, class, class, class, class, class, class> class Cont  //6arg
      , typename V, typename A, class P0, class P1, class P2, class P3, class P4, class P5
      , class U>
      struct container_rebind<Cont<V, A, P0, P1, P2, P3, P4, P5>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4, P5> type;
   };

   template <template <class, class, class, class, class, class, class, class, class> class Cont  //7arg
      , typename V, typename A, class P0, class P1, class P2, class P3, class P4, class P5, class P6
      , class U>
      struct container_rebind<Cont<V, A, P0, P1, P2, P3, P4, P5, P6>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4, P5, P6> type;
   };

   template <template <class, class, class, class, class, class, class, class, class, class> class Cont  //8arg
      , typename V, typename A, class P0, class P1, class P2, class P3, class P4, class P5, class P6, class P7
      , class U>
      struct container_rebind<Cont<V, A, P0, P1, P2, P3, P4, P5, P6, P7>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4, P5, P6, P7> type;
   };

   template <template <class, class, class, class, class, class, class, class, class, class, class> class Cont  //9arg
      , typename V, typename A, class P0, class P1, class P2, class P3, class P4, class P5, class P6, class P7, class P8
      , class U>
      struct container_rebind<Cont<V, A, P0, P1, P2, P3, P4, P5, P6, P7, P8>, U>
   {
      typedef Cont<U, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4, P5, P6, P7, P8> type;
   };

   //For small_vector/static_vector
   template <template <class, std::size_t> class Cont  //0arg
      , typename V, std::size_t N
      , class U>
      struct container_rebind<Cont<V, N>, U>
   {
      typedef Cont<U, N> type;
   };

   template <template <class, std::size_t, class> class Cont  //0arg
      , typename V, std::size_t N, typename A
      , class U>
      struct container_rebind<Cont<V, N, A>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type> type;
   };

   template <template <class, std::size_t, class, class> class Cont  //1arg
      , typename V, std::size_t N, typename A, class P0
      , class U>
      struct container_rebind<Cont<V, N, A, P0>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0> type;
   };

   template <template <class, std::size_t, class, class, class> class Cont  //2arg
      , typename V, std::size_t N, typename A, class P0, class P1
      , class U>
      struct container_rebind<Cont<V, N, A, P0, P1>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1> type;
   };

   template <template <class, std::size_t, class, class, class, class> class Cont  //3arg
      , typename V, std::size_t N, typename A, class P0, class P1, class P2
      , class U>
      struct container_rebind<Cont<V, N, A, P0, P1, P2>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2> type;
   };

   template <template <class, std::size_t, class, class, class, class, class> class Cont  //4arg
      , typename V, std::size_t N, typename A, class P0, class P1, class P2, class P3
      , class U>
      struct container_rebind<Cont<V, N, A, P0, P1, P2, P3>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3> type;
   };

   template <template <class, std::size_t, class, class, class, class, class, class> class Cont  //5arg
      , typename V, std::size_t N, typename A, class P0, class P1, class P2, class P3, class P4
      , class U>
      struct container_rebind<Cont<V, N, A, P0, P1, P2, P3, P4>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4> type;
   };

   template <template <class, std::size_t, class, class, class, class, class, class, class> class Cont  //6arg
      , typename V, std::size_t N, typename A, class P0, class P1, class P2, class P3, class P4, class P5
      , class U>
      struct container_rebind<Cont<V, N, A, P0, P1, P2, P3, P4, P5>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4, P5> type;
   };

   template <template <class, std::size_t, class, class, class, class, class, class, class, class> class Cont  //7arg
      , typename V, std::size_t N, typename A, class P0, class P1, class P2, class P3, class P4, class P5, class P6
      , class U>
      struct container_rebind<Cont<V, N, A, P0, P1, P2, P3, P4, P5, P6>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4, P5, P6> type;
   };

   template <template <class, std::size_t, class, class, class, class, class, class, class, class, class> class Cont  //8arg
      , typename V, std::size_t N, typename A, class P0, class P1, class P2, class P3, class P4, class P5, class P6, class P7
      , class U>
      struct container_rebind<Cont<V, N, A, P0, P1, P2, P3, P4, P5, P6, P7>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4, P5, P6, P7> type;
   };

   template <template <class, std::size_t, class, class, class, class, class, class, class, class, class, class> class Cont  //9arg
      , typename V, std::size_t N, typename A, class P0, class P1, class P2, class P3, class P4, class P5, class P6, class P7, class P8
      , class U>
      struct container_rebind<Cont<V, N, A, P0, P1, P2, P3, P4, P5, P6, P7, P8>, U>
   {
      typedef Cont<U, N, typename allocator_traits<A>::template portable_rebind_alloc<U>::type, P0, P1, P2, P3, P4, P5, P6, P7, P8> type;
   };

#endif   //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

}  //namespace dtl {
}  //namespace container {
}  //namespace boost {

#endif   //#ifndef BOOST_CONTAINER_DETAIL_CONTAINER_REBIND_HPP
