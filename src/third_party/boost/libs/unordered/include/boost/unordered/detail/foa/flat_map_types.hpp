// Copyright (C) 2023 Christian Mazakas
// Copyright (C) 2024 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_FOA_FLAT_MAP_TYPES_HPP
#define BOOST_UNORDERED_DETAIL_FOA_FLAT_MAP_TYPES_HPP

#include <boost/unordered/detail/foa/types_constructibility.hpp>

#include <boost/core/allocator_access.hpp>

namespace boost {
  namespace unordered {
    namespace detail {
      namespace foa {
        template <class Key, class T> struct flat_map_types
        {
          using key_type = Key;
          using mapped_type = T;
          using raw_key_type = typename std::remove_const<Key>::type;
          using raw_mapped_type = typename std::remove_const<T>::type;

          using init_type = std::pair<raw_key_type, raw_mapped_type>;
          using moved_type = std::pair<raw_key_type&&, raw_mapped_type&&>;
          using value_type = std::pair<Key const, T>;

          using element_type = value_type;

          using types = flat_map_types<Key, T>;
          using constructibility_checker = map_types_constructibility<types>;

          static value_type& value_from(element_type& x) { return x; }

          template <class K, class V>
          static raw_key_type const& extract(std::pair<K, V> const& kv)
          {
            return kv.first;
          }

          static moved_type move(init_type& x)
          {
            return {std::move(x.first), std::move(x.second)};
          }

          static moved_type move(element_type& x)
          {
            // TODO: we probably need to launder here
            return {std::move(const_cast<raw_key_type&>(x.first)),
              std::move(const_cast<raw_mapped_type&>(x.second))};
          }

          template <class A, class... Args>
          static void construct(A& al, init_type* p, Args&&... args)
          {
            constructibility_checker::check(al, p, std::forward<Args>(args)...);
            boost::allocator_construct(al, p, std::forward<Args>(args)...);
          }

          template <class A, class... Args>
          static void construct(A& al, value_type* p, Args&&... args)
          {
            constructibility_checker::check(al, p, std::forward<Args>(args)...);
            boost::allocator_construct(al, p, std::forward<Args>(args)...);
          }

          template <class A, class... Args>
          static void construct(A& al, key_type* p, Args&&... args)
          {
            constructibility_checker::check(al, p, std::forward<Args>(args)...);
            boost::allocator_construct(al, p, std::forward<Args>(args)...);
          }

          template <class A> static void destroy(A& al, init_type* p) noexcept
          {
            boost::allocator_destroy(al, p);
          }

          template <class A> static void destroy(A& al, value_type* p) noexcept
          {
            boost::allocator_destroy(al, p);
          }

          template <class A> static void destroy(A& al, key_type* p) noexcept
          {
            boost::allocator_destroy(al, p);
          }
        };
      } // namespace foa
    } // namespace detail
  } // namespace unordered
} // namespace boost

#endif // BOOST_UNORDERED_DETAIL_FOA_FLAT_MAP_TYPES_HPP
