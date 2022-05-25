//
// experimental/detail/channel_receive_op.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_RECEIVE_OP_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_RECEIVE_OP_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/bind_handler.hpp>
#include <boost/asio/detail/handler_alloc_helpers.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/detail/channel_handler.hpp>
#include <boost/asio/experimental/detail/channel_operation.hpp>
#include <boost/asio/experimental/detail/channel_payload.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename Payload>
class channel_receive : public channel_operation
{
public:
  void complete(Payload payload)
  {
    func_(this, complete_op, &payload);
  }

protected:
  channel_receive(func_type func)
    : channel_operation(func)
  {
  }
};

template <typename Payload, typename Handler, typename IoExecutor>
class channel_receive_op : public channel_receive<Payload>
{
public:
  BOOST_ASIO_DEFINE_HANDLER_PTR(channel_receive_op);

  template <typename... Args>
  channel_receive_op(Handler& handler, const IoExecutor& io_ex)
    : channel_receive<Payload>(&channel_receive_op::do_action),
      handler_(BOOST_ASIO_MOVE_CAST(Handler)(handler)),
      work_(handler_, io_ex)
  {
  }

  static void do_action(channel_operation* base,
      channel_operation::action a, void* v)
  {
    // Take ownership of the operation object.
    channel_receive_op* o(static_cast<channel_receive_op*>(base));
    ptr p = { boost::asio::detail::addressof(o->handler_), o, o };

    BOOST_ASIO_HANDLER_COMPLETION((*o));

    // Take ownership of the operation's outstanding work.
    channel_operation::handler_work<Handler, IoExecutor> w(
        BOOST_ASIO_MOVE_CAST2(channel_operation::handler_work<
          Handler, IoExecutor>)(o->work_));

    // Make a copy of the handler so that the memory can be deallocated before
    // the handler is posted. Even if we're not about to post the handler, a
    // sub-object of the handler may be the true owner of the memory associated
    // with the handler. Consequently, a local copy of the handler is required
    // to ensure that any owning sub-object remains valid until after we have
    // deallocated the memory here.
    if (a == channel_operation::complete_op)
    {
      Payload* payload = static_cast<Payload*>(v);
      channel_handler<Payload, Handler> handler(
          BOOST_ASIO_MOVE_CAST(Payload)(*payload), o->handler_);
      p.h = boost::asio::detail::addressof(handler.handler_);
      p.reset();
      BOOST_ASIO_HANDLER_INVOCATION_BEGIN(());
      w.complete(handler, handler.handler_);
      BOOST_ASIO_HANDLER_INVOCATION_END;
    }
    else
    {
      boost::asio::detail::binder0<Handler> handler(o->handler_);
      p.h = boost::asio::detail::addressof(handler.handler_);
      p.reset();
    }
  }

private:
  Handler handler_;
  channel_operation::handler_work<Handler, IoExecutor> work_;
};

} // namespace detail
} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_RECEIVE_OP_HPP
