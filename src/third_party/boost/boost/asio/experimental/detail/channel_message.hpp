//
// experimental/detail/channel_message.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_MESSAGE_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_MESSAGE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <tuple>
#include <boost/asio/detail/type_traits.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename Signature>
class channel_message;

template <typename R>
class channel_message<R()>
{
public:
  channel_message(int)
  {
  }

  template <typename Handler>
  void receive(Handler& handler)
  {
    BOOST_ASIO_MOVE_OR_LVALUE(Handler)(handler)();
  }
};

template <typename R, typename Arg0>
class channel_message<R(Arg0)>
{
public:
  template <typename T0>
  channel_message(int, BOOST_ASIO_MOVE_ARG(T0) t0)
    : arg0_(BOOST_ASIO_MOVE_CAST(T0)(t0))
  {
  }

  template <typename Handler>
  void receive(Handler& handler)
  {
    BOOST_ASIO_MOVE_OR_LVALUE(Handler)(handler)(
        BOOST_ASIO_MOVE_CAST(arg0_type)(arg0_));
  }

private:
  typedef typename decay<Arg0>::type arg0_type;
  arg0_type arg0_;
};

template <typename R, typename Arg0, typename Arg1>
class channel_message<R(Arg0, Arg1)>
{
public:
  template <typename T0, typename T1>
  channel_message(int, BOOST_ASIO_MOVE_ARG(T0) t0, BOOST_ASIO_MOVE_ARG(T1) t1)
    : arg0_(BOOST_ASIO_MOVE_CAST(T0)(t0)),
      arg1_(BOOST_ASIO_MOVE_CAST(T1)(t1))
  {
  }

  template <typename Handler>
  void receive(Handler& handler)
  {
    BOOST_ASIO_MOVE_OR_LVALUE(Handler)(handler)(
        BOOST_ASIO_MOVE_CAST(arg0_type)(arg0_),
        BOOST_ASIO_MOVE_CAST(arg1_type)(arg1_));
  }

private:
  typedef typename decay<Arg0>::type arg0_type;
  arg0_type arg0_;
  typedef typename decay<Arg1>::type arg1_type;
  arg1_type arg1_;
};

template <typename R, typename... Args>
class channel_message<R(Args...)>
{
public:
  template <typename... T>
  channel_message(int, BOOST_ASIO_MOVE_ARG(T)... t)
    : args_(BOOST_ASIO_MOVE_CAST(T)(t)...)
  {
  }

  template <typename Handler>
  void receive(Handler& h)
  {
    std::apply(BOOST_ASIO_MOVE_OR_LVALUE(Handler)(h),
        BOOST_ASIO_MOVE_CAST(args_type)(args_));
  }

private:
  typedef std::tuple<typename decay<Args>::type...> args_type;
  args_type args_;
};

} // namespace detail
} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_MESSAGE_HPP
