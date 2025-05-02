
// Copyright (C) 2005-2016 Daniel James
// Copyright (C) 2022 Christian Mazakas
// Copyright (C) 2024 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/unordered/detail/implementation.hpp>
#include <boost/unordered/unordered_map_fwd.hpp>

namespace boost {
  namespace unordered {
    namespace detail {
      template <typename A, typename K, typename M, typename H, typename P>
      struct map
      {
        typedef boost::unordered::detail::map<A, K, M, H, P> types;

        typedef std::pair<K const, M> value_type;
        typedef H hasher;
        typedef P key_equal;
        typedef K key_type;
        typedef K const const_key_type;

        typedef
          typename ::boost::unordered::detail::rebind_wrap<A, value_type>::type
            value_allocator;
        typedef boost::unordered::detail::allocator_traits<value_allocator>
          value_allocator_traits;

        typedef boost::unordered::detail::table<types> table;
        typedef boost::unordered::detail::map_extractor<value_type> extractor;

        typedef typename boost::allocator_void_pointer<value_allocator>::type
          void_pointer;

        typedef boost::unordered::node_handle_map<
          node<value_type, void_pointer>, K, M, A>
          node_type;

        typedef typename table::iterator iterator;
        typedef boost::unordered::insert_return_type_map<iterator, node_type> insert_return_type;
      };
    }
  }
}
