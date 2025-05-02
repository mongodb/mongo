
// Copyright (C) 2008-2011 Daniel James.
// Copyright (C) 2022-2023 Christian Mazakas
// Copyright (C) 2024 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_MAP_FWD_HPP_INCLUDED
#define BOOST_UNORDERED_MAP_FWD_HPP_INCLUDED

#include <boost/config.hpp>
#if defined(BOOST_HAS_PRAGMA_ONCE)
#pragma once
#endif

#include <boost/container_hash/hash_fwd.hpp>
#include <functional>
#include <memory>

#ifndef BOOST_NO_CXX17_HDR_MEMORY_RESOURCE
#include <memory_resource>
#endif

namespace boost {
  namespace unordered {
    template <class K, class T, class H = boost::hash<K>,
      class P = std::equal_to<K>,
      class A = std::allocator<std::pair<const K, T> > >
    class unordered_map;

    template <class K, class T, class H, class P, class A>
    inline bool operator==(
      unordered_map<K, T, H, P, A> const&, unordered_map<K, T, H, P, A> const&);
    template <class K, class T, class H, class P, class A>
    inline bool operator!=(
      unordered_map<K, T, H, P, A> const&, unordered_map<K, T, H, P, A> const&);
    template <class K, class T, class H, class P, class A>
    inline void swap(unordered_map<K, T, H, P, A>& m1,
      unordered_map<K, T, H, P, A>& m2) noexcept(noexcept(m1.swap(m2)));

    template <class K, class T, class H, class P, class A, class Predicate>
    typename unordered_map<K, T, H, P, A>::size_type erase_if(
      unordered_map<K, T, H, P, A>& c, Predicate pred);

    template <class K, class T, class H = boost::hash<K>,
      class P = std::equal_to<K>,
      class A = std::allocator<std::pair<const K, T> > >
    class unordered_multimap;

    template <class K, class T, class H, class P, class A>
    inline bool operator==(unordered_multimap<K, T, H, P, A> const&,
      unordered_multimap<K, T, H, P, A> const&);
    template <class K, class T, class H, class P, class A>
    inline bool operator!=(unordered_multimap<K, T, H, P, A> const&,
      unordered_multimap<K, T, H, P, A> const&);
    template <class K, class T, class H, class P, class A>
    inline void swap(unordered_multimap<K, T, H, P, A>& m1,
      unordered_multimap<K, T, H, P, A>& m2) noexcept(noexcept(m1.swap(m2)));

    template <class K, class T, class H, class P, class A, class Predicate>
    typename unordered_multimap<K, T, H, P, A>::size_type erase_if(
      unordered_multimap<K, T, H, P, A>& c, Predicate pred);

    template <class N, class K, class T, class A> class node_handle_map;
    template <class Iter, class NodeType> struct insert_return_type_map;

#ifndef BOOST_NO_CXX17_HDR_MEMORY_RESOURCE
    namespace pmr {
      template <class K, class T, class H = boost::hash<K>,
        class P = std::equal_to<K> >
      using unordered_map = boost::unordered::unordered_map<K, T, H, P,
        std::pmr::polymorphic_allocator<std::pair<const K, T> > >;

      template <class K, class T, class H = boost::hash<K>,
        class P = std::equal_to<K> >
      using unordered_multimap = boost::unordered::unordered_multimap<K, T, H,
        P, std::pmr::polymorphic_allocator<std::pair<const K, T> > >;
    } // namespace pmr
#endif
  } // namespace unordered

  using boost::unordered::unordered_map;
  using boost::unordered::unordered_multimap;
} // namespace boost

#endif
