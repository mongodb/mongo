
// Copyright (C) 2022 Christian Mazakas
// Copyright (C) 2024 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_FLAT_MAP_FWD_HPP_INCLUDED
#define BOOST_UNORDERED_FLAT_MAP_FWD_HPP_INCLUDED

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
    template <class Key, class T, class Hash = boost::hash<Key>,
      class KeyEqual = std::equal_to<Key>,
      class Allocator = std::allocator<std::pair<const Key, T> > >
    class unordered_flat_map;

    template <class Key, class T, class Hash, class KeyEqual, class Allocator>
    bool operator==(
      unordered_flat_map<Key, T, Hash, KeyEqual, Allocator> const& lhs,
      unordered_flat_map<Key, T, Hash, KeyEqual, Allocator> const& rhs);

    template <class Key, class T, class Hash, class KeyEqual, class Allocator>
    bool operator!=(
      unordered_flat_map<Key, T, Hash, KeyEqual, Allocator> const& lhs,
      unordered_flat_map<Key, T, Hash, KeyEqual, Allocator> const& rhs);

    template <class Key, class T, class Hash, class KeyEqual, class Allocator>
    void swap(unordered_flat_map<Key, T, Hash, KeyEqual, Allocator>& lhs,
      unordered_flat_map<Key, T, Hash, KeyEqual, Allocator>& rhs)
      noexcept(noexcept(lhs.swap(rhs)));

#ifndef BOOST_NO_CXX17_HDR_MEMORY_RESOURCE
    namespace pmr {
      template <class Key, class T, class Hash = boost::hash<Key>,
        class KeyEqual = std::equal_to<Key> >
      using unordered_flat_map =
        boost::unordered::unordered_flat_map<Key, T, Hash, KeyEqual,
          std::pmr::polymorphic_allocator<std::pair<const Key, T> > >;
    } // namespace pmr
#endif
  } // namespace unordered

  using boost::unordered::unordered_flat_map;
} // namespace boost

#endif
