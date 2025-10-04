//
// experimental/concurrent_channel.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_CONCURRENT_CHANNEL_HPP
#define BOOST_ASIO_EXPERIMENTAL_CONCURRENT_CHANNEL_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/execution/executor.hpp>
#include <boost/asio/is_executor.hpp>
#include <boost/asio/experimental/basic_concurrent_channel.hpp>
#include <boost/asio/experimental/channel_traits.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename ExecutorOrSignature, typename = void>
struct concurrent_channel_type
{
  template <typename... Signatures>
  struct inner
  {
    typedef basic_concurrent_channel<any_io_executor, channel_traits<>,
        ExecutorOrSignature, Signatures...> type;
  };
};

template <typename ExecutorOrSignature>
struct concurrent_channel_type<ExecutorOrSignature,
    enable_if_t<
      is_executor<ExecutorOrSignature>::value
        || execution::is_executor<ExecutorOrSignature>::value
    >>
{
  template <typename... Signatures>
  struct inner
  {
    typedef basic_concurrent_channel<ExecutorOrSignature,
        channel_traits<>, Signatures...> type;
  };
};

} // namespace detail

/// Template type alias for common use of channel.
#if defined(GENERATING_DOCUMENTATION)
template <typename ExecutorOrSignature, typename... Signatures>
using concurrent_channel = basic_concurrent_channel<
    specified_executor_or_any_io_executor, channel_traits<>, signatures...>;
#else // defined(GENERATING_DOCUMENTATION)
template <typename ExecutorOrSignature, typename... Signatures>
using concurrent_channel = typename detail::concurrent_channel_type<
    ExecutorOrSignature>::template inner<Signatures...>::type;
#endif // defined(GENERATING_DOCUMENTATION)

} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_CONCURRENT_CHANNEL_HPP
