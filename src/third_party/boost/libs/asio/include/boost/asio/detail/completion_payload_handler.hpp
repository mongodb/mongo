//
// detail/completion_payload_handler.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_COMPLETION_PAYLOAD_HANDLER_HPP
#define BOOST_ASIO_DETAIL_COMPLETION_PAYLOAD_HANDLER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/associator.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

template <typename Payload, typename Handler>
class completion_payload_handler
{
public:
  completion_payload_handler(Payload&& p, Handler& h)
    : payload_(static_cast<Payload&&>(p)),
      handler_(static_cast<Handler&&>(h))
  {
  }

  void operator()()
  {
    payload_.receive(handler_);
  }

  Handler& handler()
  {
    return handler_;
  }

//private:
  Payload payload_;
  Handler handler_;
};

} // namespace detail

template <template <typename, typename> class Associator,
    typename Payload, typename Handler, typename DefaultCandidate>
struct associator<Associator,
    detail::completion_payload_handler<Payload, Handler>,
    DefaultCandidate>
  : Associator<Handler, DefaultCandidate>
{
  static typename Associator<Handler, DefaultCandidate>::type get(
      const detail::completion_payload_handler<Payload, Handler>& h) noexcept
  {
    return Associator<Handler, DefaultCandidate>::get(h.handler_);
  }

  static auto get(
      const detail::completion_payload_handler<Payload, Handler>& h,
      const DefaultCandidate& c) noexcept
    -> decltype(Associator<Handler, DefaultCandidate>::get(h.handler_, c))
  {
    return Associator<Handler, DefaultCandidate>::get(h.handler_, c);
  }
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_DETAIL_COMPLETION_PAYLOAD_HANDLER_HPP
