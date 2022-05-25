//
// experimental/detail/channel_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_SERVICE_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/detail/mutex.hpp>
#include <boost/asio/detail/op_queue.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/experimental/detail/channel_message.hpp>
#include <boost/asio/experimental/detail/channel_receive_op.hpp>
#include <boost/asio/experimental/detail/channel_send_op.hpp>
#include <boost/asio/experimental/detail/has_signature.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename Mutex>
class channel_service
  : public boost::asio::detail::execution_context_service_base<
      channel_service<Mutex> >
{
public:
  // Possible states for a channel end.
  enum state
  {
    buffer = 0,
    waiter = 1,
    block = 2,
    closed = 3
  };

  // The base implementation type of all channels.
  struct base_implementation_type
  {
    // Default constructor.
    base_implementation_type()
      : receive_state_(block),
        send_state_(block),
        max_buffer_size_(0),
        next_(0),
        prev_(0)
    {
    }

    // The current state of the channel.
    state receive_state_ : 16;
    state send_state_ : 16;

    // The maximum number of elements that may be buffered in the channel.
    std::size_t max_buffer_size_;

    // The operations that are waiting on the channel.
    boost::asio::detail::op_queue<channel_operation> waiters_;

    // Pointers to adjacent channel implementations in linked list.
    base_implementation_type* next_;
    base_implementation_type* prev_;

    // The mutex type to protect the internal implementation.
    mutable Mutex mutex_;
  };

  // The implementation for a specific value type.
  template <typename Traits, typename... Signatures>
  struct implementation_type;

  // Constructor.
  channel_service(execution_context& ctx);

  // Destroy all user-defined handler objects owned by the service.
  void shutdown();

  // Construct a new channel implementation.
  void construct(base_implementation_type& impl, std::size_t max_buffer_size);

  // Destroy a channel implementation.
  template <typename Traits, typename... Signatures>
  void destroy(implementation_type<Traits, Signatures...>& impl);

  // Move-construct a new channel implementation.
  template <typename Traits, typename... Signatures>
  void move_construct(implementation_type<Traits, Signatures...>& impl,
      implementation_type<Traits, Signatures...>& other_impl);

  // Move-assign from another channel implementation.
  template <typename Traits, typename... Signatures>
  void move_assign(implementation_type<Traits, Signatures...>& impl,
      channel_service& other_service,
      implementation_type<Traits, Signatures...>& other_impl);

  // Get the capacity of the channel.
  std::size_t capacity(
      const base_implementation_type& impl) const BOOST_ASIO_NOEXCEPT;

  // Determine whether the channel is open.
  bool is_open(const base_implementation_type& impl) const BOOST_ASIO_NOEXCEPT;

  // Reset the channel to its initial state.
  template <typename Traits, typename... Signatures>
  void reset(implementation_type<Traits, Signatures...>& impl);

  // Close the channel.
  template <typename Traits, typename... Signatures>
  void close(implementation_type<Traits, Signatures...>& impl);

  // Cancel all operations associated with the channel.
  template <typename Traits, typename... Signatures>
  void cancel(implementation_type<Traits, Signatures...>& impl);

  // Cancel the operation associated with the channel that has the given key.
  template <typename Traits, typename... Signatures>
  void cancel_by_key(implementation_type<Traits, Signatures...>& impl,
      void* cancellation_key);

  // Determine whether a value can be read from the channel without blocking.
  bool ready(const base_implementation_type& impl) const BOOST_ASIO_NOEXCEPT;

  // Synchronously send a new value into the channel.
  template <typename Message, typename Traits,
      typename... Signatures, typename... Args>
  bool try_send(implementation_type<Traits, Signatures...>& impl,
      BOOST_ASIO_MOVE_ARG(Args)... args);

  // Synchronously send a number of new values into the channel.
  template <typename Message, typename Traits,
      typename... Signatures, typename... Args>
  std::size_t try_send_n(implementation_type<Traits, Signatures...>& impl,
      std::size_t count, BOOST_ASIO_MOVE_ARG(Args)... args);

  // Asynchronously send a new value into the channel.
  template <typename Traits, typename... Signatures,
      typename Handler, typename IoExecutor>
  void async_send(implementation_type<Traits, Signatures...>& impl,
      BOOST_ASIO_MOVE_ARG2(typename implementation_type<
        Traits, Signatures...>::payload_type) payload,
      Handler& handler, const IoExecutor& io_ex)
  {
    typename associated_cancellation_slot<Handler>::type slot
      = boost::asio::get_associated_cancellation_slot(handler);

    // Allocate and construct an operation to wrap the handler.
    typedef channel_send_op<
      typename implementation_type<Traits, Signatures...>::payload_type,
        Handler, IoExecutor> op;
    typename op::ptr p = { boost::asio::detail::addressof(handler),
      op::ptr::allocate(handler), 0 };
    p.p = new (p.v) op(BOOST_ASIO_MOVE_CAST2(typename implementation_type<
          Traits, Signatures...>::payload_type)(payload), handler, io_ex);

    // Optionally register for per-operation cancellation.
    if (slot.is_connected())
    {
      p.p->cancellation_key_ =
        &slot.template emplace<op_cancellation<Traits, Signatures...> >(
            this, &impl);
    }

    BOOST_ASIO_HANDLER_CREATION((this->context(), *p.p,
          "channel", &impl, 0, "async_send"));

    start_send_op(impl, p.p);
    p.v = p.p = 0;
  }

  // Synchronously receive a value from the channel.
  template <typename Traits, typename... Signatures, typename Handler>
  bool try_receive(implementation_type<Traits, Signatures...>& impl,
      BOOST_ASIO_MOVE_ARG(Handler) handler);

  // Asynchronously send a new value into the channel.
  // Asynchronously receive a value from the channel.
  template <typename Traits, typename... Signatures,
      typename Handler, typename IoExecutor>
  void async_receive(implementation_type<Traits, Signatures...>& impl,
      Handler& handler, const IoExecutor& io_ex)
  {
    typename associated_cancellation_slot<Handler>::type slot
      = boost::asio::get_associated_cancellation_slot(handler);

    // Allocate and construct an operation to wrap the handler.
    typedef channel_receive_op<
      typename implementation_type<Traits, Signatures...>::payload_type,
        Handler, IoExecutor> op;
    typename op::ptr p = { boost::asio::detail::addressof(handler),
      op::ptr::allocate(handler), 0 };
    p.p = new (p.v) op(handler, io_ex);

    // Optionally register for per-operation cancellation.
    if (slot.is_connected())
    {
      p.p->cancellation_key_ =
        &slot.template emplace<op_cancellation<Traits, Signatures...> >(
            this, &impl);
    }

    BOOST_ASIO_HANDLER_CREATION((this->context(), *p.p,
          "channel", &impl, 0, "async_receive"));

    start_receive_op(impl, p.p);
    p.v = p.p = 0;
  }

private:
  // Helper function object to handle a closed notification.
  template <typename Payload, typename Signature>
  struct complete_receive
  {
    explicit complete_receive(channel_receive<Payload>* op)
      : op_(op)
    {
    }

    template <typename... Args>
    void operator()(BOOST_ASIO_MOVE_ARG(Args)... args)
    {
      op_->complete(
          channel_message<Signature>(0,
            BOOST_ASIO_MOVE_CAST(Args)(args)...));
    }

    channel_receive<Payload>* op_;
  };

  // Destroy a base channel implementation.
  void base_destroy(base_implementation_type& impl);

  // Helper function to start an asynchronous put operation.
  template <typename Traits, typename... Signatures>
  void start_send_op(implementation_type<Traits, Signatures...>& impl,
      channel_send<typename implementation_type<
        Traits, Signatures...>::payload_type>* send_op);

  // Helper function to start an asynchronous get operation.
  template <typename Traits, typename... Signatures>
  void start_receive_op(implementation_type<Traits, Signatures...>& impl,
      channel_receive<typename implementation_type<
        Traits, Signatures...>::payload_type>* receive_op);

  // Helper class used to implement per-operation cancellation.
  template <typename Traits, typename... Signatures>
  class op_cancellation
  {
  public:
    op_cancellation(channel_service* s,
        implementation_type<Traits, Signatures...>* impl)
      : service_(s),
        impl_(impl)
    {
    }

    void operator()(cancellation_type_t type)
    {
      if (!!(type &
            (cancellation_type::terminal
              | cancellation_type::partial
              | cancellation_type::total)))
      {
        service_->cancel_by_key(*impl_, this);
      }
    }

  private:
    channel_service* service_;
    implementation_type<Traits, Signatures...>* impl_;
  };

  // Mutex to protect access to the linked list of implementations.
  boost::asio::detail::mutex mutex_;

  // The head of a linked list of all implementations.
  base_implementation_type* impl_list_;
};

// The implementation for a specific value type.
template <typename Mutex>
template <typename Traits, typename... Signatures>
struct channel_service<Mutex>::implementation_type : base_implementation_type
{
  // The traits type associated with the channel.
  typedef typename Traits::template rebind<Signatures...>::other traits_type;

  // Type of an element stored in the buffer.
  typedef typename conditional<
      has_signature<
        typename traits_type::receive_cancelled_signature,
        Signatures...
      >::value,
      typename conditional<
        has_signature<
          typename traits_type::receive_closed_signature,
          Signatures...
        >::value,
        channel_payload<Signatures...>,
        channel_payload<
          Signatures...,
          typename traits_type::receive_closed_signature
        >
      >::type,
      typename conditional<
        has_signature<
          typename traits_type::receive_closed_signature,
          Signatures...,
          typename traits_type::receive_cancelled_signature
        >::value,
        channel_payload<
          Signatures...,
          typename traits_type::receive_cancelled_signature
        >,
        channel_payload<
          Signatures...,
          typename traits_type::receive_cancelled_signature,
          typename traits_type::receive_closed_signature
        >
      >::type
    >::type payload_type;

  // Move from another buffer.
  void buffer_move_from(implementation_type& other)
  {
    buffer_ = BOOST_ASIO_MOVE_CAST(
        typename traits_type::template container<
          payload_type>::type)(other.buffer_);
    other.buffer_clear();
  }

  // Get number of buffered elements.
  std::size_t buffer_size() const
  {
    return buffer_.size();
  }

  // Push a new value to the back of the buffer.
  void buffer_push(payload_type payload)
  {
    buffer_.push_back(BOOST_ASIO_MOVE_CAST(payload_type)(payload));
  }

  // Push new values to the back of the buffer.
  std::size_t buffer_push_n(std::size_t count, payload_type payload)
  {
    std::size_t i = 0;
    for (; i < count && buffer_.size() < this->max_buffer_size_; ++i)
      buffer_.push_back(payload);
    return i;
  }

  // Get the element at the front of the buffer.
  payload_type buffer_front()
  {
    return BOOST_ASIO_MOVE_CAST(payload_type)(buffer_.front());
  }

  // Pop a value from the front of the buffer.
  void buffer_pop()
  {
    buffer_.pop_front();
  }

  // Clear all buffered values.
  void buffer_clear()
  {
    buffer_.clear();
  }

private:
  // Buffered values.
  typename traits_type::template container<payload_type>::type buffer_;
};

// The implementation for a void value type.
template <typename Mutex>
template <typename Traits, typename R>
struct channel_service<Mutex>::implementation_type<Traits, R()>
  : channel_service::base_implementation_type
{
  // The traits type associated with the channel.
  typedef typename Traits::template rebind<R()>::other traits_type;

  // Type of an element stored in the buffer.
  typedef typename conditional<
      has_signature<
        typename traits_type::receive_cancelled_signature,
        R()
      >::value,
      typename conditional<
        has_signature<
          typename traits_type::receive_closed_signature,
          R()
        >::value,
        channel_payload<R()>,
        channel_payload<
          R(),
          typename traits_type::receive_closed_signature
        >
      >::type,
      typename conditional<
        has_signature<
          typename traits_type::receive_closed_signature,
          R(),
          typename traits_type::receive_cancelled_signature
        >::value,
        channel_payload<
          R(),
          typename traits_type::receive_cancelled_signature
        >,
        channel_payload<
          R(),
          typename traits_type::receive_cancelled_signature,
          typename traits_type::receive_closed_signature
        >
      >::type
    >::type payload_type;

  // Construct with empty buffer.
  implementation_type()
    : buffer_(0)
  {
  }

  // Move from another buffer.
  void buffer_move_from(implementation_type& other)
  {
    buffer_ = other.buffer_;
    other.buffer_ = 0;
  }

  // Get number of buffered elements.
  std::size_t buffer_size() const
  {
    return buffer_;
  }

  // Push a new value to the back of the buffer.
  void buffer_push(payload_type)
  {
    ++buffer_;
  }

  // Push new values to the back of the buffer.
  std::size_t buffer_push_n(std::size_t count, payload_type)
  {
    std::size_t available = this->max_buffer_size_ - buffer_;
    count = (count < available) ? count : available;
    buffer_ += count;
    return count;
  }

  // Get the element at the front of the buffer.
  payload_type buffer_front()
  {
    return payload_type(channel_message<R()>(0));
  }

  // Pop a value from the front of the buffer.
  void buffer_pop()
  {
    --buffer_;
  }

  // Clear all values from the buffer.
  void buffer_clear()
  {
    buffer_ = 0;
  }

private:
  // Number of buffered "values".
  std::size_t buffer_;
};

} // namespace detail
} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#include <boost/asio/experimental/detail/impl/channel_service.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_SERVICE_HPP
