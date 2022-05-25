//
// experimental/detail/impl/channel_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_IMPL_CHANNEL_SERVICE_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_IMPL_CHANNEL_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename Mutex>
inline channel_service<Mutex>::channel_service(execution_context& ctx)
  : boost::asio::detail::execution_context_service_base<channel_service>(ctx),
    mutex_(),
    impl_list_(0)
{
}

template <typename Mutex>
inline void channel_service<Mutex>::shutdown()
{
  // Abandon all pending operations.
  boost::asio::detail::op_queue<channel_operation> ops;
  boost::asio::detail::mutex::scoped_lock lock(mutex_);
  base_implementation_type* impl = impl_list_;
  while (impl)
  {
    ops.push(impl->waiters_);
    impl = impl->next_;
  }
}

template <typename Mutex>
inline void channel_service<Mutex>::construct(
    channel_service<Mutex>::base_implementation_type& impl,
    std::size_t max_buffer_size)
{
  impl.max_buffer_size_ = max_buffer_size;
  impl.receive_state_ = block;
  impl.send_state_ = max_buffer_size ? buffer : block;

  // Insert implementation into linked list of all implementations.
  boost::asio::detail::mutex::scoped_lock lock(mutex_);
  impl.next_ = impl_list_;
  impl.prev_ = 0;
  if (impl_list_)
    impl_list_->prev_ = &impl;
  impl_list_ = &impl;
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::destroy(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl)
{
  cancel(impl);
  base_destroy(impl);
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::move_construct(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl,
    channel_service<Mutex>::implementation_type<
      Traits, Signatures...>& other_impl)
{
  impl.max_buffer_size_ = other_impl.max_buffer_size_;
  impl.receive_state_ = other_impl.receive_state_;
  other_impl.receive_state_ = block;
  impl.send_state_ = other_impl.send_state_;
  other_impl.send_state_ = other_impl.max_buffer_size_ ? buffer : block;
  impl.buffer_move_from(other_impl);

  // Insert implementation into linked list of all implementations.
  boost::asio::detail::mutex::scoped_lock lock(mutex_);
  impl.next_ = impl_list_;
  impl.prev_ = 0;
  if (impl_list_)
    impl_list_->prev_ = &impl;
  impl_list_ = &impl;
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::move_assign(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl,
    channel_service& other_service,
    channel_service<Mutex>::implementation_type<
      Traits, Signatures...>& other_impl)
{
  cancel(impl);

  if (this != &other_service)
  {
    // Remove implementation from linked list of all implementations.
    boost::asio::detail::mutex::scoped_lock lock(mutex_);
    if (impl_list_ == &impl)
      impl_list_ = impl.next_;
    if (impl.prev_)
      impl.prev_->next_ = impl.next_;
    if (impl.next_)
      impl.next_->prev_= impl.prev_;
    impl.next_ = 0;
    impl.prev_ = 0;
  }

  impl.max_buffer_size_ = other_impl.max_buffer_size_;
  impl.receive_state_ = other_impl.receive_state_;
  other_impl.receive_state_ = block;
  impl.send_state_ = other_impl.send_state_;
  other_impl.send_state_ = other_impl.max_buffer_size_ ? buffer : block;
  impl.buffer_move_from(other_impl);

  if (this != &other_service)
  {
    // Insert implementation into linked list of all implementations.
    boost::asio::detail::mutex::scoped_lock lock(other_service.mutex_);
    impl.next_ = other_service.impl_list_;
    impl.prev_ = 0;
    if (other_service.impl_list_)
      other_service.impl_list_->prev_ = &impl;
    other_service.impl_list_ = &impl;
  }
}

template <typename Mutex>
inline void channel_service<Mutex>::base_destroy(
    channel_service<Mutex>::base_implementation_type& impl)
{
  // Remove implementation from linked list of all implementations.
  boost::asio::detail::mutex::scoped_lock lock(mutex_);
  if (impl_list_ == &impl)
    impl_list_ = impl.next_;
  if (impl.prev_)
    impl.prev_->next_ = impl.next_;
  if (impl.next_)
    impl.next_->prev_= impl.prev_;
  impl.next_ = 0;
  impl.prev_ = 0;
}

template <typename Mutex>
inline std::size_t channel_service<Mutex>::capacity(
    const channel_service<Mutex>::base_implementation_type& impl)
  const BOOST_ASIO_NOEXCEPT
{
  typename Mutex::scoped_lock lock(impl.mutex_);

  return impl.max_buffer_size_;
}

template <typename Mutex>
inline bool channel_service<Mutex>::is_open(
    const channel_service<Mutex>::base_implementation_type& impl)
  const BOOST_ASIO_NOEXCEPT
{
  typename Mutex::scoped_lock lock(impl.mutex_);

  return impl.send_state_ != closed;
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::reset(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl)
{
  cancel(impl);

  typename Mutex::scoped_lock lock(impl.mutex_);

  impl.receive_state_ = block;
  impl.send_state_ = impl.max_buffer_size_ ? buffer : block;
  impl.buffer_clear();
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::close(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl)
{
  typedef typename implementation_type<Traits,
      Signatures...>::traits_type traits_type;
  typedef typename implementation_type<Traits,
      Signatures...>::payload_type payload_type;

  typename Mutex::scoped_lock lock(impl.mutex_);

  if (impl.receive_state_ == block)
  {
    while (channel_operation* op = impl.waiters_.front())
    {
      impl.waiters_.pop();
      traits_type::invoke_receive_closed(
          complete_receive<payload_type,
            typename traits_type::receive_closed_signature>(
              static_cast<channel_receive<payload_type>*>(op)));
    }
  }

  impl.send_state_ = closed;
  impl.receive_state_ = closed;
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::cancel(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl)
{
  typedef typename implementation_type<Traits,
      Signatures...>::traits_type traits_type;
  typedef typename implementation_type<Traits,
      Signatures...>::payload_type payload_type;

  typename Mutex::scoped_lock lock(impl.mutex_);

  while (channel_operation* op = impl.waiters_.front())
  {
    if (impl.send_state_ == block)
    {
      impl.waiters_.pop();
      static_cast<channel_send<payload_type>*>(op)->cancel();
    }
    else
    {
      impl.waiters_.pop();
      traits_type::invoke_receive_cancelled(
          complete_receive<payload_type,
            typename traits_type::receive_cancelled_signature>(
              static_cast<channel_receive<payload_type>*>(op)));
    }
  }

  if (impl.receive_state_ == waiter)
    impl.receive_state_ = block;
  if (impl.send_state_ == waiter)
    impl.send_state_ = block;
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::cancel_by_key(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl,
    void* cancellation_key)
{
  typedef typename implementation_type<Traits,
      Signatures...>::traits_type traits_type;
  typedef typename implementation_type<Traits,
      Signatures...>::payload_type payload_type;

  typename Mutex::scoped_lock lock(impl.mutex_);

  boost::asio::detail::op_queue<channel_operation> other_ops;
  while (channel_operation* op = impl.waiters_.front())
  {
    if (op->cancellation_key_ == cancellation_key)
    {
      if (impl.send_state_ == block)
      {
        impl.waiters_.pop();
        static_cast<channel_send<payload_type>*>(op)->cancel();
      }
      else
      {
        impl.waiters_.pop();
        traits_type::invoke_receive_cancelled(
            complete_receive<payload_type,
              typename traits_type::receive_cancelled_signature>(
                static_cast<channel_receive<payload_type>*>(op)));
      }
    }
    else
    {
      impl.waiters_.pop();
      other_ops.push(op);
    }
  }
  impl.waiters_.push(other_ops);

  if (impl.waiters_.empty())
  {
    if (impl.receive_state_ == waiter)
      impl.receive_state_ = block;
    if (impl.send_state_ == waiter)
      impl.send_state_ = block;
  }
}

template <typename Mutex>
inline bool channel_service<Mutex>::ready(
    const channel_service<Mutex>::base_implementation_type& impl)
  const BOOST_ASIO_NOEXCEPT
{
  typename Mutex::scoped_lock lock(impl.mutex_);

  return impl.receive_state_ != block;
}

template <typename Mutex>
template <typename Message, typename Traits,
    typename... Signatures, typename... Args>
bool channel_service<Mutex>::try_send(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl,
    BOOST_ASIO_MOVE_ARG(Args)... args)
{
  typedef typename implementation_type<Traits,
      Signatures...>::payload_type payload_type;

  typename Mutex::scoped_lock lock(impl.mutex_);

  switch (impl.send_state_)
  {
  case block:
    {
      return false;
    }
  case buffer:
    {
      impl.buffer_push(Message(0, BOOST_ASIO_MOVE_CAST(Args)(args)...));
      impl.receive_state_ = buffer;
      if (impl.buffer_size() == impl.max_buffer_size_)
        impl.send_state_ = block;
      return true;
    }
  case waiter:
    {
      payload_type payload(Message(0, BOOST_ASIO_MOVE_CAST(Args)(args)...));
      channel_receive<payload_type>* receive_op =
        static_cast<channel_receive<payload_type>*>(impl.waiters_.front());
      impl.waiters_.pop();
      receive_op->complete(BOOST_ASIO_MOVE_CAST(payload_type)(payload));
      if (impl.waiters_.empty())
        impl.send_state_ = impl.max_buffer_size_ ? buffer : block;
      return true;
    }
  case closed:
  default:
    {
      return false;
    }
  }
}

template <typename Mutex>
template <typename Message, typename Traits,
    typename... Signatures, typename... Args>
std::size_t channel_service<Mutex>::try_send_n(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl,
		std::size_t count, BOOST_ASIO_MOVE_ARG(Args)... args)
{
  typedef typename implementation_type<Traits,
      Signatures...>::payload_type payload_type;

  typename Mutex::scoped_lock lock(impl.mutex_);

  if (count == 0)
    return 0;

  switch (impl.send_state_)
  {
  case block:
    return 0;
  case buffer:
  case waiter:
    break;
  case closed:
  default:
    return 0;
  }

  payload_type payload(Message(0, BOOST_ASIO_MOVE_CAST(Args)(args)...));

  for (std::size_t i = 0; i < count; ++i)
  {
    switch (impl.send_state_)
    {
    case block:
      {
        return i;
      }
    case buffer:
      {
        i += impl.buffer_push_n(count - i,
            BOOST_ASIO_MOVE_CAST(payload_type)(payload));
        impl.receive_state_ = buffer;
        if (impl.buffer_size() == impl.max_buffer_size_)
          impl.send_state_ = block;
        return i;
      }
    case waiter:
      {
        channel_receive<payload_type>* receive_op =
          static_cast<channel_receive<payload_type>*>(impl.waiters_.front());
        impl.waiters_.pop();
        receive_op->complete(payload);
        if (impl.waiters_.empty())
          impl.send_state_ = impl.max_buffer_size_ ? buffer : block;
        break;
      }
    case closed:
    default:
      {
        return i;
      }
    }
  }

  return count;
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::start_send_op(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl,
		channel_send<typename implementation_type<
			Traits, Signatures...>::payload_type>* send_op)
{
  typedef typename implementation_type<Traits,
      Signatures...>::payload_type payload_type;

  typename Mutex::scoped_lock lock(impl.mutex_);

  switch (impl.send_state_)
  {
  case block:
    {
      impl.waiters_.push(send_op);
      if (impl.receive_state_ == block)
        impl.receive_state_ = waiter;
      return;
    }
  case buffer:
    {
      impl.buffer_push(send_op->get_payload());
      impl.receive_state_ = buffer;
      if (impl.buffer_size() == impl.max_buffer_size_)
        impl.send_state_ = block;
      send_op->complete();
      break;
    }
  case waiter:
    {
      channel_receive<payload_type>* receive_op =
        static_cast<channel_receive<payload_type>*>(impl.waiters_.front());
      impl.waiters_.pop();
      receive_op->complete(send_op->get_payload());
      if (impl.waiters_.empty())
        impl.send_state_ = impl.max_buffer_size_ ? buffer : block;
      send_op->complete();
      break;
    }
  case closed:
  default:
    {
      send_op->close();
      break;
    }
  }
}

template <typename Mutex>
template <typename Traits, typename... Signatures, typename Handler>
bool channel_service<Mutex>::try_receive(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl,
		BOOST_ASIO_MOVE_ARG(Handler) handler)
{
  typedef typename implementation_type<Traits,
      Signatures...>::payload_type payload_type;

  typename Mutex::scoped_lock lock(impl.mutex_);

  switch (impl.receive_state_)
  {
  case block:
    {
      return false;
    }
  case buffer:
    {
      payload_type payload(impl.buffer_front());
      if (channel_send<payload_type>* send_op =
          static_cast<channel_send<payload_type>*>(impl.waiters_.front()))
      {
        impl.buffer_pop();
        impl.buffer_push(send_op->get_payload());
        impl.waiters_.pop();
        send_op->complete();
      }
      else
      {
        impl.buffer_pop();
        if (impl.buffer_size() == 0)
          impl.receive_state_ = (impl.send_state_ == closed) ? closed : block;
        impl.send_state_ = (impl.send_state_ == closed) ? closed : buffer;
      }
      lock.unlock();
      boost::asio::detail::non_const_lvalue<Handler> handler2(handler);
      channel_handler<payload_type, typename decay<Handler>::type>(
          BOOST_ASIO_MOVE_CAST(payload_type)(payload), handler2.value)();
      return true;
    }
  case waiter:
    {
      channel_send<payload_type>* send_op =
        static_cast<channel_send<payload_type>*>(impl.waiters_.front());
      payload_type payload = send_op->get_payload();
      impl.waiters_.pop();
      send_op->complete();
      if (impl.waiters_.front() == 0)
        impl.receive_state_ = (impl.send_state_ == closed) ? closed : block;
      lock.unlock();
      boost::asio::detail::non_const_lvalue<Handler> handler2(handler);
      channel_handler<payload_type, typename decay<Handler>::type>(
          BOOST_ASIO_MOVE_CAST(payload_type)(payload), handler2.value)();
      return true;
    }
  case closed:
  default:
    {
      return false;
    }
  }
}

template <typename Mutex>
template <typename Traits, typename... Signatures>
void channel_service<Mutex>::start_receive_op(
    channel_service<Mutex>::implementation_type<Traits, Signatures...>& impl,
		channel_receive<typename implementation_type<
			Traits, Signatures...>::payload_type>* receive_op)
{
  typedef typename implementation_type<Traits,
      Signatures...>::traits_type traits_type;
  typedef typename implementation_type<Traits,
      Signatures...>::payload_type payload_type;

  typename Mutex::scoped_lock lock(impl.mutex_);

  switch (impl.receive_state_)
  {
  case block:
    {
      impl.waiters_.push(receive_op);
      if (impl.send_state_ != closed)
        impl.send_state_ = waiter;
      return;
    }
  case buffer:
    {
      receive_op->complete(impl.buffer_front());
      if (channel_send<payload_type>* send_op =
          static_cast<channel_send<payload_type>*>(impl.waiters_.front()))
      {
        impl.buffer_pop();
        impl.buffer_push(send_op->get_payload());
        impl.waiters_.pop();
        send_op->complete();
      }
      else
      {
        impl.buffer_pop();
        if (impl.buffer_size() == 0)
          impl.receive_state_ = (impl.send_state_ == closed) ? closed : block;
        impl.send_state_ = (impl.send_state_ == closed) ? closed : buffer;
      }
      break;
    }
  case waiter:
    {
      channel_send<payload_type>* send_op =
        static_cast<channel_send<payload_type>*>(impl.waiters_.front());
      payload_type payload = send_op->get_payload();
      impl.waiters_.pop();
      send_op->complete();
      receive_op->complete(BOOST_ASIO_MOVE_CAST(payload_type)(payload));
      if (impl.waiters_.front() == 0)
        impl.receive_state_ = (impl.send_state_ == closed) ? closed : block;
      break;
    }
  case closed:
  default:
    {
      traits_type::invoke_receive_closed(
          complete_receive<payload_type,
            typename traits_type::receive_closed_signature>(receive_op));
      break;
    }
  }
}

} // namespace detail
} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_IMPL_CHANNEL_SERVICE_HPP
