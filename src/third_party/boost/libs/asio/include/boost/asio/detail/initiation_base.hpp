//
// detail/initiation_base.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_INITIATION_BASE_HPP
#define BOOST_ASIO_DETAIL_INITIATION_BASE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/type_traits.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

template <typename Initiation, typename = void>
class initiation_base : public Initiation
{
public:
  template <typename I>
  explicit initiation_base(I&& initiation)
    : Initiation(static_cast<I&&>(initiation))
  {
  }
};

template <typename Initiation>
class initiation_base<Initiation, enable_if_t<!is_class<Initiation>::value>>
{
public:
  template <typename I>
  explicit initiation_base(I&& initiation)
    : initiation_(static_cast<I&&>(initiation))
  {
  }

  template <typename... Args>
  void operator()(Args&&... args) const
  {
    initiation_(static_cast<Args&&>(args)...);
  }

private:
  Initiation initiation_;
};

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_DETAIL_INITIATION_BASE_HPP
