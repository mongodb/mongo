// Copyright (C) 2023 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_FOA_FLAT_SET_TYPES_HPP
#define BOOST_UNORDERED_DETAIL_FOA_FLAT_SET_TYPES_HPP

#include <boost/unordered/detail/foa/types_constructibility.hpp>

#include <boost/core/allocator_access.hpp>

namespace boost {
  namespace unordered {
    namespace detail {
      namespace foa {
        template <class Key> struct flat_set_types
        {
          using key_type = Key;
          using init_type = Key;
          using value_type = Key;

          static Key const& extract(value_type const& key) { return key; }

          using element_type = value_type;

          using types = flat_set_types<Key>;
          using constructibility_checker = set_types_constructibility<types>;

          static Key& value_from(element_type& x) { return x; }

          static element_type&& move(element_type& x) { return std::move(x); }

          template <class A, class... Args>
          static void construct(A& al, value_type* p, Args&&... args)
          {
            constructibility_checker::check(al, p, std::forward<Args>(args)...);
            boost::allocator_construct(al, p, std::forward<Args>(args)...);
          }

          template <class A> static void destroy(A& al, value_type* p) noexcept
          {
            boost::allocator_destroy(al, p);
          }
        };
      } // namespace foa
    } // namespace detail
  } // namespace unordered
} // namespace boost

#endif // BOOST_UNORDERED_DETAIL_FOA_FLAT_SET_TYPES_HPP
