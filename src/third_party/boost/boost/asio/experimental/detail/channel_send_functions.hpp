//
// experimental/detail/channel_send_functions.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_SEND_FUNCTIONS_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_SEND_FUNCTIONS_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/experimental/detail/channel_message.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename Derived, typename Executor, typename... Signatures>
class channel_send_functions;

template <typename Derived, typename Executor, typename R, typename... Args>
class channel_send_functions<Derived, Executor, R(Args...)>
{
public:
  template <typename... Args2>
  typename enable_if<
    is_constructible<detail::channel_message<R(Args...)>, int, Args2...>::value,
    bool
  >::type try_send(BOOST_ASIO_MOVE_ARG(Args2)... args)
  {
    typedef typename detail::channel_message<R(Args...)> message_type;
    Derived* self = static_cast<Derived*>(this);
    return self->service_->template try_send<message_type>(
        self->impl_, BOOST_ASIO_MOVE_CAST(Args2)(args)...);
  }

  template <typename... Args2>
  typename enable_if<
    is_constructible<detail::channel_message<R(Args...)>, int, Args2...>::value,
    std::size_t
  >::type try_send_n(std::size_t count, BOOST_ASIO_MOVE_ARG(Args2)... args)
  {
    typedef typename detail::channel_message<R(Args...)> message_type;
    Derived* self = static_cast<Derived*>(this);
    return self->service_->template try_send_n<message_type>(
        self->impl_, count, BOOST_ASIO_MOVE_CAST(Args2)(args)...);
  }

  template <
      BOOST_ASIO_COMPLETION_TOKEN_FOR(void (boost::system::error_code))
        CompletionToken BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(Executor)>
  auto async_send(Args... args,
      BOOST_ASIO_MOVE_ARG(CompletionToken) token
        BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(Executor))
  {
    typedef typename Derived::payload_type payload_type;
    typedef typename detail::channel_message<R(Args...)> message_type;
    Derived* self = static_cast<Derived*>(this);
    return async_initiate<CompletionToken, void (boost::system::error_code)>(
        typename Derived::initiate_async_send(self), token,
        payload_type(message_type(0, BOOST_ASIO_MOVE_CAST(Args)(args)...)));
  }
};

template <typename Derived, typename Executor,
    typename R, typename... Args, typename... Signatures>
class channel_send_functions<Derived, Executor, R(Args...), Signatures...> :
  public channel_send_functions<Derived, Executor, Signatures...>
{
public:
  using channel_send_functions<Derived, Executor, Signatures...>::try_send;
  using channel_send_functions<Derived, Executor, Signatures...>::async_send;

  template <typename... Args2>
  typename enable_if<
    is_constructible<detail::channel_message<R(Args...)>, int, Args2...>::value,
    bool
  >::type try_send(BOOST_ASIO_MOVE_ARG(Args2)... args)
  {
    typedef typename detail::channel_message<R(Args...)> message_type;
    Derived* self = static_cast<Derived*>(this);
    return self->service_->template try_send<message_type>(
        self->impl_, BOOST_ASIO_MOVE_CAST(Args2)(args)...);
  }

  template <typename... Args2>
  typename enable_if<
    is_constructible<detail::channel_message<R(Args...)>, int, Args2...>::value,
    std::size_t
  >::type try_send_n(std::size_t count, BOOST_ASIO_MOVE_ARG(Args2)... args)
  {
    typedef typename detail::channel_message<R(Args...)> message_type;
    Derived* self = static_cast<Derived*>(this);
    return self->service_->template try_send_n<message_type>(
        self->impl_, count, BOOST_ASIO_MOVE_CAST(Args2)(args)...);
  }

  template <
      BOOST_ASIO_COMPLETION_TOKEN_FOR(void (boost::system::error_code))
        CompletionToken BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(Executor)>
  auto async_send(Args... args,
      BOOST_ASIO_MOVE_ARG(CompletionToken) token
        BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(Executor))
  {
    typedef typename Derived::payload_type payload_type;
    typedef typename detail::channel_message<R(Args...)> message_type;
    Derived* self = static_cast<Derived*>(this);
    return async_initiate<CompletionToken, void (boost::system::error_code)>(
        typename Derived::initiate_async_send(self), token,
        payload_type(message_type(0, BOOST_ASIO_MOVE_CAST(Args)(args)...)));
  }
};

} // namespace detail
} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_SEND_FUNCTIONS_HPP
