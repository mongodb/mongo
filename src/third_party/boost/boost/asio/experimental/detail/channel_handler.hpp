//
// experimental/detail/channel_handler.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_HANDLER_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_HANDLER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/associator.hpp>
#include <boost/asio/experimental/detail/channel_payload.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename Payload, typename Handler>
class channel_handler
{
public:
  channel_handler(BOOST_ASIO_MOVE_ARG(Payload) p, Handler& h)
    : payload_(BOOST_ASIO_MOVE_CAST(Payload)(p)),
      handler_(BOOST_ASIO_MOVE_CAST(Handler)(h))
  {
  }

  void operator()()
  {
    payload_.receive(handler_);
  }

//private:
  Payload payload_;
  Handler handler_;
};

} // namespace detail
} // namespace experimental

template <template <typename, typename> class Associator,
    typename Payload, typename Handler, typename DefaultCandidate>
struct associator<Associator,
    experimental::detail::channel_handler<Payload, Handler>,
    DefaultCandidate>
  : Associator<Handler, DefaultCandidate>
{
  static typename Associator<Handler, DefaultCandidate>::type get(
      const experimental::detail::channel_handler<Payload, Handler>& h,
      const DefaultCandidate& c = DefaultCandidate()) BOOST_ASIO_NOEXCEPT
  {
    return Associator<Handler, DefaultCandidate>::get(h.handler_, c);
  }
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_HANDLER_HPP
