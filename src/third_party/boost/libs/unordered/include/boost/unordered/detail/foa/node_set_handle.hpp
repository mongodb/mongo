/* Copyright 2023 Christian Mazakas.
 * Copyright 2024 Joaquin M Lopez Munoz. 
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_FOA_NODE_SET_HANDLE_HPP
#define BOOST_UNORDERED_DETAIL_FOA_NODE_SET_HANDLE_HPP

#include <boost/unordered/detail/foa/node_handle.hpp>

namespace boost{
namespace unordered{
namespace detail{
namespace foa{

template <class TypePolicy, class Allocator>
struct node_set_handle
    : public detail::foa::node_handle_base<TypePolicy, Allocator>
{
private:
  using base_type = detail::foa::node_handle_base<TypePolicy, Allocator>;

  using typename base_type::type_policy;

public:
  using value_type = typename TypePolicy::value_type;

  constexpr node_set_handle() noexcept = default;
  node_set_handle(node_set_handle&& nh) noexcept = default;
  node_set_handle& operator=(node_set_handle&&) noexcept = default;

  value_type& value() const
  {
    BOOST_ASSERT(!this->empty());
    return const_cast<value_type&>(this->data());
  }
};

}
}
}
}

#endif // BOOST_UNORDERED_DETAIL_FOA_NODE_SET_HANDLE_HPP
