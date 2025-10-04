/* Fast open-addressing, node-based concurrent hashmap.
 *
 * Copyright 2023 Christian Mazakas.
 * Copyright 2024 Braden Ganetsky.
 * Copyright 2024 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_CONCURRENT_NODE_MAP_FWD_HPP
#define BOOST_UNORDERED_CONCURRENT_NODE_MAP_FWD_HPP

#include <boost/config.hpp>
#include <boost/container_hash/hash_fwd.hpp>

#include <functional>
#include <memory>

#ifndef BOOST_NO_CXX17_HDR_MEMORY_RESOURCE
#include <memory_resource>
#endif

namespace boost {
  namespace unordered {

    template <class Key, class T, class Hash = boost::hash<Key>,
      class Pred = std::equal_to<Key>,
      class Allocator = std::allocator<std::pair<Key const, T> > >
    class concurrent_node_map;

    template <class Key, class T, class Hash, class KeyEqual, class Allocator>
    bool operator==(
      concurrent_node_map<Key, T, Hash, KeyEqual, Allocator> const& lhs,
      concurrent_node_map<Key, T, Hash, KeyEqual, Allocator> const& rhs);

    template <class Key, class T, class Hash, class KeyEqual, class Allocator>
    bool operator!=(
      concurrent_node_map<Key, T, Hash, KeyEqual, Allocator> const& lhs,
      concurrent_node_map<Key, T, Hash, KeyEqual, Allocator> const& rhs);

    template <class Key, class T, class Hash, class Pred, class Alloc>
    void swap(concurrent_node_map<Key, T, Hash, Pred, Alloc>& x,
      concurrent_node_map<Key, T, Hash, Pred, Alloc>& y)
      noexcept(noexcept(x.swap(y)));

    template <class K, class T, class H, class P, class A, class Predicate>
    typename concurrent_node_map<K, T, H, P, A>::size_type erase_if(
      concurrent_node_map<K, T, H, P, A>& c, Predicate pred);

#ifndef BOOST_NO_CXX17_HDR_MEMORY_RESOURCE
    namespace pmr {
      template <class Key, class T, class Hash = boost::hash<Key>,
        class Pred = std::equal_to<Key> >
      using concurrent_node_map = boost::unordered::concurrent_node_map<Key, T,
        Hash, Pred, std::pmr::polymorphic_allocator<std::pair<Key const, T> > >;
    } // namespace pmr
#endif

  } // namespace unordered

  using boost::unordered::concurrent_node_map;
} // namespace boost

#endif // BOOST_UNORDERED_CONCURRENT_NODE_MAP_FWD_HPP
