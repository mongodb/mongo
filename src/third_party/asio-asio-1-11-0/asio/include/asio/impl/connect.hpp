//
// impl/connect.hpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_CONNECT_HPP
#define ASIO_IMPL_CONNECT_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/associated_allocator.hpp"
#include "asio/associated_executor.hpp"
#include "asio/detail/bind_handler.hpp"
#include "asio/detail/consuming_buffers.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_cont_helpers.hpp"
#include "asio/detail/handler_invoke_helpers.hpp"
#include "asio/detail/handler_type_requirements.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"
#include "asio/post.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

namespace detail
{
  struct default_connect_condition
  {
    template <typename Iterator>
    Iterator operator()(const asio::error_code&, Iterator next)
    {
      return next;
    }
  };
}

template <typename Protocol, typename SocketService, typename Iterator>
Iterator connect(basic_socket<Protocol, SocketService>& s, Iterator begin)
{
  asio::error_code ec;
  Iterator result = connect(s, begin, ec);
  asio::detail::throw_error(ec, "connect");
  return result;
}

template <typename Protocol, typename SocketService, typename Iterator>
inline Iterator connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, asio::error_code& ec)
{
  return connect(s, begin, Iterator(), detail::default_connect_condition(), ec);
}

template <typename Protocol, typename SocketService, typename Iterator>
Iterator connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, Iterator end)
{
  asio::error_code ec;
  Iterator result = connect(s, begin, end, ec);
  asio::detail::throw_error(ec, "connect");
  return result;
}

template <typename Protocol, typename SocketService, typename Iterator>
inline Iterator connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, Iterator end, asio::error_code& ec)
{
  return connect(s, begin, end, detail::default_connect_condition(), ec);
}

template <typename Protocol, typename SocketService,
    typename Iterator, typename ConnectCondition>
Iterator connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, ConnectCondition connect_condition)
{
  asio::error_code ec;
  Iterator result = connect(s, begin, connect_condition, ec);
  asio::detail::throw_error(ec, "connect");
  return result;
}

template <typename Protocol, typename SocketService,
    typename Iterator, typename ConnectCondition>
inline Iterator connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, ConnectCondition connect_condition,
    asio::error_code& ec)
{
  return connect(s, begin, Iterator(), connect_condition, ec);
}

template <typename Protocol, typename SocketService,
    typename Iterator, typename ConnectCondition>
Iterator connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, Iterator end, ConnectCondition connect_condition)
{
  asio::error_code ec;
  Iterator result = connect(s, begin, end, connect_condition, ec);
  asio::detail::throw_error(ec, "connect");
  return result;
}

template <typename Protocol, typename SocketService,
    typename Iterator, typename ConnectCondition>
Iterator connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, Iterator end, ConnectCondition connect_condition,
    asio::error_code& ec)
{
  ec = asio::error_code();

  for (Iterator iter = begin; iter != end; ++iter)
  {
    iter = connect_condition(ec, iter);
    if (iter != end)
    {
      s.close(ec);
      s.connect(*iter, ec);
      if (!ec)
        return iter;
    }
  }

  if (!ec)
    ec = asio::error::not_found;

  return end;
}

namespace detail
{
  // Enable the empty base class optimisation for the connect condition.
  template <typename ConnectCondition>
  class base_from_connect_condition
  {
  protected:
    explicit base_from_connect_condition(
        const ConnectCondition& connect_condition)
      : connect_condition_(connect_condition)
    {
    }

    template <typename Iterator>
    void check_condition(const asio::error_code& ec,
        Iterator& iter, Iterator& end)
    {
      if (iter != end)
        iter = connect_condition_(ec, static_cast<const Iterator&>(iter));
    }

  private:
    ConnectCondition connect_condition_;
  };

  // The default_connect_condition implementation is essentially a no-op. This
  // template specialisation lets us eliminate all costs associated with it.
  template <>
  class base_from_connect_condition<default_connect_condition>
  {
  protected:
    explicit base_from_connect_condition(const default_connect_condition&)
    {
    }

    template <typename Iterator>
    void check_condition(const asio::error_code&, Iterator&, Iterator&)
    {
    }
  };

  template <typename Protocol, typename SocketService, typename Iterator,
      typename ConnectCondition, typename ComposedConnectHandler>
  class connect_op : base_from_connect_condition<ConnectCondition>
  {
  public:
    connect_op(basic_socket<Protocol, SocketService>& sock,
        const Iterator& begin, const Iterator& end,
        const ConnectCondition& connect_condition,
        ComposedConnectHandler& handler)
      : base_from_connect_condition<ConnectCondition>(connect_condition),
        socket_(sock),
        iter_(begin),
        end_(end),
        start_(0),
        handler_(ASIO_MOVE_CAST(ComposedConnectHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    connect_op(const connect_op& other)
      : base_from_connect_condition<ConnectCondition>(other),
        socket_(other.socket_),
        iter_(other.iter_),
        end_(other.end_),
        start_(other.start_),
        handler_(other.handler_)
    {
    }

    connect_op(connect_op&& other)
      : base_from_connect_condition<ConnectCondition>(other),
        socket_(other.socket_),
        iter_(other.iter_),
        end_(other.end_),
        start_(other.start_),
        handler_(ASIO_MOVE_CAST(ComposedConnectHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(asio::error_code ec, int start = 0)
    {
      switch (start_ = start)
      {
        case 1:
        for (;;)
        {
          this->check_condition(ec, iter_, end_);

          if (iter_ != end_)
          {
            socket_.close(ec);
            socket_.async_connect(*iter_,
                ASIO_MOVE_CAST(connect_op)(*this));
            return;
          }

          if (start)
          {
            ec = asio::error::not_found;
            asio::post(socket_.get_executor(),
                detail::bind_handler(*this, ec));
            return;
          }

          default:

          if (iter_ == end_)
            break;

          if (!socket_.is_open())
          {
            ec = asio::error::operation_aborted;
            break;
          }

          if (!ec)
            break;

          ++iter_;
        }

        handler_(static_cast<const asio::error_code&>(ec),
            static_cast<const Iterator&>(iter_));
      }
    }

  //private:
    basic_socket<Protocol, SocketService>& socket_;
    Iterator iter_;
    Iterator end_;
    int start_;
    ComposedConnectHandler handler_;
  };

  template <typename Protocol, typename SocketService, typename Iterator,
      typename ConnectCondition, typename ComposedConnectHandler>
  inline void* asio_handler_allocate(std::size_t size,
      connect_op<Protocol, SocketService, Iterator,
        ConnectCondition, ComposedConnectHandler>* this_handler)
  {
    return asio_handler_alloc_helpers::allocate(
        size, this_handler->handler_);
  }

  template <typename Protocol, typename SocketService, typename Iterator,
      typename ConnectCondition, typename ComposedConnectHandler>
  inline void asio_handler_deallocate(void* pointer, std::size_t size,
      connect_op<Protocol, SocketService, Iterator,
        ConnectCondition, ComposedConnectHandler>* this_handler)
  {
    asio_handler_alloc_helpers::deallocate(
        pointer, size, this_handler->handler_);
  }

  template <typename Protocol, typename SocketService, typename Iterator,
      typename ConnectCondition, typename ComposedConnectHandler>
  inline bool asio_handler_is_continuation(
      connect_op<Protocol, SocketService, Iterator,
        ConnectCondition, ComposedConnectHandler>* this_handler)
  {
    return asio_handler_cont_helpers::is_continuation(
        this_handler->handler_);
  }

  template <typename Function, typename Protocol,
      typename SocketService, typename Iterator,
      typename ConnectCondition, typename ComposedConnectHandler>
  inline void asio_handler_invoke(Function& function,
      connect_op<Protocol, SocketService, Iterator,
        ConnectCondition, ComposedConnectHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }

  template <typename Function, typename Protocol,
      typename SocketService, typename Iterator,
      typename ConnectCondition, typename ComposedConnectHandler>
  inline void asio_handler_invoke(const Function& function,
      connect_op<Protocol, SocketService, Iterator,
        ConnectCondition, ComposedConnectHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }
} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

template <typename Protocol, typename SocketService,
    typename Iterator, typename ConnectCondition,
    typename ComposedConnectHandler, typename Allocator>
struct associated_allocator<
    detail::connect_op<Protocol, SocketService, Iterator,
      ConnectCondition, ComposedConnectHandler>,
    Allocator>
{
  typedef typename associated_allocator<
      ComposedConnectHandler, Allocator>::type type;

  static type get(
      const detail::connect_op<Protocol, SocketService,
        Iterator, ConnectCondition, ComposedConnectHandler>& h,
      const Allocator& a = Allocator()) ASIO_NOEXCEPT
  {
    return associated_allocator<ComposedConnectHandler,
        Allocator>::get(h.handler_, a);
  }
};

template <typename Protocol, typename SocketService,
    typename Iterator, typename ConnectCondition,
    typename ComposedConnectHandler, typename Executor>
struct associated_executor<
    detail::connect_op<Protocol, SocketService, Iterator,
      ConnectCondition, ComposedConnectHandler>,
    Executor>
{
  typedef typename associated_executor<
      ComposedConnectHandler, Executor>::type type;

  static type get(
      const detail::connect_op<Protocol, SocketService,
        Iterator, ConnectCondition, ComposedConnectHandler>& h,
      const Executor& ex = Executor()) ASIO_NOEXCEPT
  {
    return associated_executor<ComposedConnectHandler,
        Executor>::get(h.handler_, ex);
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

template <typename Protocol, typename SocketService,
    typename Iterator, typename ComposedConnectHandler>
inline ASIO_INITFN_RESULT_TYPE(ComposedConnectHandler,
    void (asio::error_code, Iterator))
async_connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, ASIO_MOVE_ARG(ComposedConnectHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a ComposedConnectHandler.
  ASIO_COMPOSED_CONNECT_HANDLER_CHECK(
      ComposedConnectHandler, handler, Iterator) type_check;

  async_completion<ComposedConnectHandler,
    void (asio::error_code, Iterator)> init(handler);

  detail::connect_op<Protocol, SocketService, Iterator,
    detail::default_connect_condition, ASIO_HANDLER_TYPE(
      ComposedConnectHandler, void (asio::error_code, Iterator))>(s,
        begin, Iterator(), detail::default_connect_condition(), init.handler)(
          asio::error_code(), 1);

  return init.result.get();
}

template <typename Protocol, typename SocketService,
    typename Iterator, typename ComposedConnectHandler>
inline ASIO_INITFN_RESULT_TYPE(ComposedConnectHandler,
    void (asio::error_code, Iterator))
async_connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, Iterator end,
    ASIO_MOVE_ARG(ComposedConnectHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a ComposedConnectHandler.
  ASIO_COMPOSED_CONNECT_HANDLER_CHECK(
      ComposedConnectHandler, handler, Iterator) type_check;

  async_completion<ComposedConnectHandler,
    void (asio::error_code, Iterator)> init(handler);

  detail::connect_op<Protocol, SocketService, Iterator,
    detail::default_connect_condition, ASIO_HANDLER_TYPE(
      ComposedConnectHandler, void (asio::error_code, Iterator))>(s,
        begin, end, detail::default_connect_condition(), init.handler)(
          asio::error_code(), 1);

  return init.result.get();
}

template <typename Protocol, typename SocketService, typename Iterator,
    typename ConnectCondition, typename ComposedConnectHandler>
inline ASIO_INITFN_RESULT_TYPE(ComposedConnectHandler,
    void (asio::error_code, Iterator))
async_connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, ConnectCondition connect_condition,
    ASIO_MOVE_ARG(ComposedConnectHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a ComposedConnectHandler.
  ASIO_COMPOSED_CONNECT_HANDLER_CHECK(
      ComposedConnectHandler, handler, Iterator) type_check;

  async_completion<ComposedConnectHandler,
    void (asio::error_code, Iterator)> init(handler);

  detail::connect_op<Protocol, SocketService, Iterator,
    ConnectCondition, ASIO_HANDLER_TYPE(
      ComposedConnectHandler, void (asio::error_code, Iterator))>(s,
        begin, Iterator(), connect_condition, init.handler)(
          asio::error_code(), 1);

  return init.result.get();
}

template <typename Protocol, typename SocketService, typename Iterator,
    typename ConnectCondition, typename ComposedConnectHandler>
inline ASIO_INITFN_RESULT_TYPE(ComposedConnectHandler,
    void (asio::error_code, Iterator))
async_connect(basic_socket<Protocol, SocketService>& s,
    Iterator begin, Iterator end, ConnectCondition connect_condition,
    ASIO_MOVE_ARG(ComposedConnectHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a ComposedConnectHandler.
  ASIO_COMPOSED_CONNECT_HANDLER_CHECK(
      ComposedConnectHandler, handler, Iterator) type_check;

  async_completion<ComposedConnectHandler,
    void (asio::error_code, Iterator)> init(handler);

  detail::connect_op<Protocol, SocketService, Iterator,
    ConnectCondition, ASIO_HANDLER_TYPE(
      ComposedConnectHandler, void (asio::error_code, Iterator))>(s,
        begin, end, connect_condition, init.handler)(
          asio::error_code(), 1);

  return init.result.get();
}

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_CONNECT_HPP
