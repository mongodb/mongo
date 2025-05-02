/* Copyright 2023 Christian Mazakas.
 * Copyright 2024 Joaquin M Lopez Munoz. 
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_FOA_NODE_MAP_HANDLE_HPP
#define BOOST_UNORDERED_DETAIL_FOA_NODE_MAP_HANDLE_HPP

#include <boost/unordered/detail/foa/node_handle.hpp>

namespace boost{
namespace unordered{
namespace detail{
namespace foa{

template <class TypePolicy, class Allocator>
struct node_map_handle
    : public node_handle_base<TypePolicy, Allocator>
{
private:
  using base_type = node_handle_base<TypePolicy, Allocator>;

  using typename base_type::type_policy;

public:
  using key_type = typename TypePolicy::key_type;
  using mapped_type = typename TypePolicy::mapped_type;

  constexpr node_map_handle() noexcept = default;
  node_map_handle(node_map_handle&& nh) noexcept = default;

  node_map_handle& operator=(node_map_handle&&) noexcept = default;

  key_type& key() const
  {
    BOOST_ASSERT(!this->empty());
    return const_cast<key_type&>(this->data().first);
  }

  mapped_type& mapped() const
  {
    BOOST_ASSERT(!this->empty());
    return const_cast<mapped_type&>(this->data().second);
  }
};

}
}
}
}

#endif // BOOST_UNORDERED_DETAIL_FOA_NODE_MAP_HANDLE_HPP
