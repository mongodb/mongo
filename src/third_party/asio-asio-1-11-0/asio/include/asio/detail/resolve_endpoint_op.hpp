//
// detail/resolve_endpoint_op.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_RESOLVER_ENDPOINT_OP_HPP
#define ASIO_DETAIL_RESOLVER_ENDPOINT_OP_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/error.hpp"
#include "asio/io_service.hpp"
#include "asio/ip/basic_resolver_iterator.hpp"
#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_invoke_helpers.hpp"
#include "asio/detail/memory.hpp"
#include "asio/detail/operation.hpp"
#include "asio/detail/socket_ops.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

template <typename Protocol, typename Handler>
class resolve_endpoint_op : public operation
{
public:
  ASIO_DEFINE_HANDLER_PTR(resolve_endpoint_op);

  typedef typename Protocol::endpoint endpoint_type;
  typedef asio::ip::basic_resolver_iterator<Protocol> iterator_type;

  resolve_endpoint_op(socket_ops::weak_cancel_token_type cancel_token,
      const endpoint_type& endpoint, io_service_impl& ios, Handler& handler)
    : operation(&resolve_endpoint_op::do_complete),
      cancel_token_(cancel_token),
      endpoint_(endpoint),
      io_service_impl_(ios),
      handler_(ASIO_MOVE_CAST(Handler)(handler))
  {
    handler_work<Handler>::start(handler_);
  }

  static void do_complete(void* owner, operation* base,
      const asio::error_code& /*ec*/,
      std::size_t /*bytes_transferred*/)
  {
    // Take ownership of the operation object.
    resolve_endpoint_op* o(static_cast<resolve_endpoint_op*>(base));
    ptr p = { asio::detail::addressof(o->handler_), o, o };
    handler_work<Handler> w(o->handler_);

    if (owner && owner != &o->io_service_impl_)
    {
      // The operation is being run on the worker io_service. Time to perform
      // the resolver operation.
    
      // Perform the blocking endpoint resolution operation.
      char host_name[NI_MAXHOST];
      char service_name[NI_MAXSERV];
      socket_ops::background_getnameinfo(o->cancel_token_, o->endpoint_.data(),
          o->endpoint_.size(), host_name, NI_MAXHOST, service_name, NI_MAXSERV,
          o->endpoint_.protocol().type(), o->ec_);
      o->iter_ = iterator_type::create(o->endpoint_, host_name, service_name);

      // Pass operation back to main io_service for completion.
      o->io_service_impl_.post_deferred_completion(o);
      p.v = p.p = 0;
    }
    else
    {
      // The operation has been returned to the main io_service. The completion
      // handler is ready to be delivered.

      ASIO_HANDLER_COMPLETION((o));

      // Make a copy of the handler so that the memory can be deallocated
      // before the upcall is made. Even if we're not about to make an upcall,
      // a sub-object of the handler may be the true owner of the memory
      // associated with the handler. Consequently, a local copy of the handler
      // is required to ensure that any owning sub-object remains valid until
      // after we have deallocated the memory here.
      detail::binder2<Handler, asio::error_code, iterator_type>
        handler(o->handler_, o->ec_, o->iter_);
      p.h = asio::detail::addressof(handler.handler_);
      p.reset();

      if (owner)
      {
        fenced_block b(fenced_block::half);
        ASIO_HANDLER_INVOCATION_BEGIN((handler.arg1_, "..."));
        w.complete(handler, handler.handler_);
        ASIO_HANDLER_INVOCATION_END;
      }
    }
  }

private:
  socket_ops::weak_cancel_token_type cancel_token_;
  endpoint_type endpoint_;
  io_service_impl& io_service_impl_;
  Handler handler_;
  asio::error_code ec_;
  iterator_type iter_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_RESOLVER_ENDPOINT_OP_HPP
