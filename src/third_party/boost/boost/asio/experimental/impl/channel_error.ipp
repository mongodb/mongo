//
// experimental/impl/channel_error.ipp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_IMPL_CHANNEL_ERROR_IPP
#define BOOST_ASIO_EXPERIMENTAL_IMPL_CHANNEL_ERROR_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/experimental/channel_error.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace error {
namespace detail {

class channel_category : public boost::system::error_category
{
public:
  const char* name() const BOOST_ASIO_ERROR_CATEGORY_NOEXCEPT
  {
    return "asio.channel";
  }

  std::string message(int value) const
  {
    switch (value)
    {
    case channel_closed: return "Channel closed";
    case channel_cancelled: return "Channel cancelled";
    default: return "asio.channel error";
    }
  }
};

} // namespace detail

const boost::system::error_category& get_channel_category()
{
  static detail::channel_category instance;
  return instance;
}

} // namespace error
} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_IMPL_CHANNEL_ERROR_IPP
