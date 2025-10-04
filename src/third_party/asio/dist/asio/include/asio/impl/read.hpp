//
// impl/read.hpp
// ~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_READ_HPP
#define ASIO_IMPL_READ_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <algorithm>
#include "asio/associator.hpp"
#include "asio/buffer.hpp"
#include "asio/detail/array_fwd.hpp"
#include "asio/detail/base_from_cancellation_state.hpp"
#include "asio/detail/base_from_completion_cond.hpp"
#include "asio/detail/bind_handler.hpp"
#include "asio/detail/consuming_buffers.hpp"
#include "asio/detail/dependent_type.hpp"
#include "asio/detail/handler_cont_helpers.hpp"
#include "asio/detail/handler_tracking.hpp"
#include "asio/detail/handler_type_requirements.hpp"
#include "asio/detail/non_const_lvalue.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

namespace detail
{
  template <typename SyncReadStream, typename MutableBufferSequence,
      typename MutableBufferIterator, typename CompletionCondition>
  std::size_t read_buffer_seq(SyncReadStream& s,
      const MutableBufferSequence& buffers, const MutableBufferIterator&,
      CompletionCondition completion_condition, asio::error_code& ec)
  {
    ec = asio::error_code();
    asio::detail::consuming_buffers<mutable_buffer,
        MutableBufferSequence, MutableBufferIterator> tmp(buffers);
    while (!tmp.empty())
    {
      if (std::size_t max_size = detail::adapt_completion_condition_result(
            completion_condition(ec, tmp.total_consumed())))
        tmp.consume(s.read_some(tmp.prepare(max_size), ec));
      else
        break;
    }
    return tmp.total_consumed();
  }
} // namespace detail

template <typename SyncReadStream, typename MutableBufferSequence,
    typename CompletionCondition>
std::size_t read(SyncReadStream& s, const MutableBufferSequence& buffers,
    CompletionCondition completion_condition, asio::error_code& ec,
    constraint_t<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >,
    constraint_t<
      is_completion_condition<CompletionCondition>::value
    >)
{
  return detail::read_buffer_seq(s, buffers,
      asio::buffer_sequence_begin(buffers),
      static_cast<CompletionCondition&&>(completion_condition), ec);
}

template <typename SyncReadStream, typename MutableBufferSequence>
inline std::size_t read(SyncReadStream& s, const MutableBufferSequence& buffers,
    constraint_t<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read(s, buffers, transfer_all(), ec);
  asio::detail::throw_error(ec, "read");
  return bytes_transferred;
}

template <typename SyncReadStream, typename MutableBufferSequence>
inline std::size_t read(SyncReadStream& s, const MutableBufferSequence& buffers,
    asio::error_code& ec,
    constraint_t<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >)
{
  return read(s, buffers, transfer_all(), ec);
}

template <typename SyncReadStream, typename MutableBufferSequence,
    typename CompletionCondition>
inline std::size_t read(SyncReadStream& s, const MutableBufferSequence& buffers,
    CompletionCondition completion_condition,
    constraint_t<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >,
    constraint_t<
      is_completion_condition<CompletionCondition>::value
    >)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read(s, buffers,
      static_cast<CompletionCondition&&>(completion_condition), ec);
  asio::detail::throw_error(ec, "read");
  return bytes_transferred;
}

#if !defined(ASIO_NO_DYNAMIC_BUFFER_V1)

template <typename SyncReadStream, typename DynamicBuffer_v1,
    typename CompletionCondition>
std::size_t read(SyncReadStream& s,
    DynamicBuffer_v1&& buffers,
    CompletionCondition completion_condition, asio::error_code& ec,
    constraint_t<
      is_dynamic_buffer_v1<decay_t<DynamicBuffer_v1>>::value
    >,
    constraint_t<
      !is_dynamic_buffer_v2<decay_t<DynamicBuffer_v1>>::value
    >,
    constraint_t<
      is_completion_condition<CompletionCondition>::value
    >)
{
  decay_t<DynamicBuffer_v1> b(
      static_cast<DynamicBuffer_v1&&>(buffers));

  ec = asio::error_code();
  std::size_t total_transferred = 0;
  std::size_t max_size = detail::adapt_completion_condition_result(
        completion_condition(ec, total_transferred));
  std::size_t bytes_available = std::min<std::size_t>(
        std::max<std::size_t>(512, b.capacity() - b.size()),
        std::min<std::size_t>(max_size, b.max_size() - b.size()));
  while (bytes_available > 0)
  {
    std::size_t bytes_transferred = s.read_some(b.prepare(bytes_available), ec);
    b.commit(bytes_transferred);
    total_transferred += bytes_transferred;
    max_size = detail::adapt_completion_condition_result(
          completion_condition(ec, total_transferred));
    bytes_available = std::min<std::size_t>(
          std::max<std::size_t>(512, b.capacity() - b.size()),
          std::min<std::size_t>(max_size, b.max_size() - b.size()));
  }
  return total_transferred;
}

template <typename SyncReadStream, typename DynamicBuffer_v1>
inline std::size_t read(SyncReadStream& s,
    DynamicBuffer_v1&& buffers,
    constraint_t<
      is_dynamic_buffer_v1<decay_t<DynamicBuffer_v1>>::value
    >,
    constraint_t<
      !is_dynamic_buffer_v2<decay_t<DynamicBuffer_v1>>::value
    >)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read(s,
      static_cast<DynamicBuffer_v1&&>(buffers), transfer_all(), ec);
  asio::detail::throw_error(ec, "read");
  return bytes_transferred;
}

template <typename SyncReadStream, typename DynamicBuffer_v1>
inline std::size_t read(SyncReadStream& s,
    DynamicBuffer_v1&& buffers,
    asio::error_code& ec,
    constraint_t<
      is_dynamic_buffer_v1<decay_t<DynamicBuffer_v1>>::value
    >,
    constraint_t<
      !is_dynamic_buffer_v2<decay_t<DynamicBuffer_v1>>::value
    >)
{
  return read(s, static_cast<DynamicBuffer_v1&&>(buffers),
      transfer_all(), ec);
}

template <typename SyncReadStream, typename DynamicBuffer_v1,
    typename CompletionCondition>
inline std::size_t read(SyncReadStream& s,
    DynamicBuffer_v1&& buffers,
    CompletionCondition completion_condition,
    constraint_t<
      is_dynamic_buffer_v1<decay_t<DynamicBuffer_v1>>::value
    >,
    constraint_t<
      !is_dynamic_buffer_v2<decay_t<DynamicBuffer_v1>>::value
    >,
    constraint_t<
      is_completion_condition<CompletionCondition>::value
    >)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read(s,
      static_cast<DynamicBuffer_v1&&>(buffers),
      static_cast<CompletionCondition&&>(completion_condition), ec);
  asio::detail::throw_error(ec, "read");
  return bytes_transferred;
}

#if !defined(ASIO_NO_EXTENSIONS)
#if !defined(ASIO_NO_IOSTREAM)

template <typename SyncReadStream, typename Allocator,
    typename CompletionCondition>
inline std::size_t read(SyncReadStream& s,
    asio::basic_streambuf<Allocator>& b,
    CompletionCondition completion_condition, asio::error_code& ec,
    constraint_t<
      is_completion_condition<CompletionCondition>::value
    >)
{
  return read(s, basic_streambuf_ref<Allocator>(b),
      static_cast<CompletionCondition&&>(completion_condition), ec);
}

template <typename SyncReadStream, typename Allocator>
inline std::size_t read(SyncReadStream& s,
    asio::basic_streambuf<Allocator>& b)
{
  return read(s, basic_streambuf_ref<Allocator>(b));
}

template <typename SyncReadStream, typename Allocator>
inline std::size_t read(SyncReadStream& s,
    asio::basic_streambuf<Allocator>& b,
    asio::error_code& ec)
{
  return read(s, basic_streambuf_ref<Allocator>(b), ec);
}

template <typename SyncReadStream, typename Allocator,
    typename CompletionCondition>
inline std::size_t read(SyncReadStream& s,
    asio::basic_streambuf<Allocator>& b,
    CompletionCondition completion_condition,
    constraint_t<
      is_completion_condition<CompletionCondition>::value
    >)
{
  return read(s, basic_streambuf_ref<Allocator>(b),
      static_cast<CompletionCondition&&>(completion_condition));
}

#endif // !defined(ASIO_NO_IOSTREAM)
#endif // !defined(ASIO_NO_EXTENSIONS)
#endif // !defined(ASIO_NO_DYNAMIC_BUFFER_V1)

template <typename SyncReadStream, typename DynamicBuffer_v2,
    typename CompletionCondition>
std::size_t read(SyncReadStream& s, DynamicBuffer_v2 buffers,
    CompletionCondition completion_condition, asio::error_code& ec,
    constraint_t<
      is_dynamic_buffer_v2<DynamicBuffer_v2>::value
    >,
    constraint_t<
      is_completion_condition<CompletionCondition>::value
    >)
{
  DynamicBuffer_v2& b = buffers;

  ec = asio::error_code();
  std::size_t total_transferred = 0;
  std::size_t max_size = detail::adapt_completion_condition_result(
        completion_condition(ec, total_transferred));
  std::size_t bytes_available = std::min<std::size_t>(
        std::max<std::size_t>(512, b.capacity() - b.size()),
        std::min<std::size_t>(max_size, b.max_size() - b.size()));
  while (bytes_available > 0)
  {
    std::size_t pos = b.size();
    b.grow(bytes_available);
    std::size_t bytes_transferred = s.read_some(
        b.data(pos, bytes_available), ec);
    b.shrink(bytes_available - bytes_transferred);
    total_transferred += bytes_transferred;
    max_size = detail::adapt_completion_condition_result(
          completion_condition(ec, total_transferred));
    bytes_available = std::min<std::size_t>(
          std::max<std::size_t>(512, b.capacity() - b.size()),
          std::min<std::size_t>(max_size, b.max_size() - b.size()));
  }
  return total_transferred;
}

template <typename SyncReadStream, typename DynamicBuffer_v2>
inline std::size_t read(SyncReadStream& s, DynamicBuffer_v2 buffers,
    constraint_t<
      is_dynamic_buffer_v2<DynamicBuffer_v2>::value
    >)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read(s,
      static_cast<DynamicBuffer_v2&&>(buffers), transfer_all(), ec);
  asio::detail::throw_error(ec, "read");
  return bytes_transferred;
}

template <typename SyncReadStream, typename DynamicBuffer_v2>
inline std::size_t read(SyncReadStream& s, DynamicBuffer_v2 buffers,
    asio::error_code& ec,
    constraint_t<
      is_dynamic_buffer_v2<DynamicBuffer_v2>::value
    >)
{
  return read(s, static_cast<DynamicBuffer_v2&&>(buffers),
      transfer_all(), ec);
}

template <typename SyncReadStream, typename DynamicBuffer_v2,
    typename CompletionCondition>
inline std::size_t read(SyncReadStream& s, DynamicBuffer_v2 buffers,
    CompletionCondition completion_condition,
    constraint_t<
      is_dynamic_buffer_v2<DynamicBuffer_v2>::value
    >,
    constraint_t<
      is_completion_condition<CompletionCondition>::value
    >)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read(s,
      static_cast<DynamicBuffer_v2&&>(buffers),
      static_cast<CompletionCondition&&>(completion_condition), ec);
  asio::detail::throw_error(ec, "read");
  return bytes_transferred;
}

namespace detail
{
  template <typename AsyncReadStream, typename MutableBufferSequence,
      typename MutableBufferIterator, typename CompletionCondition,
      typename ReadHandler>
  class read_op
    : public base_from_cancellation_state<ReadHandler>,
      base_from_completion_cond<CompletionCondition>
  {
  public:
    read_op(AsyncReadStream& stream, const MutableBufferSequence& buffers,
        CompletionCondition& completion_condition, ReadHandler& handler)
      : base_from_cancellation_state<ReadHandler>(
          handler, enable_partial_cancellation()),
        base_from_completion_cond<CompletionCondition>(completion_condition),
        stream_(stream),
        buffers_(buffers),
        start_(0),
        handler_(static_cast<ReadHandler&&>(handler))
    {
    }

    read_op(const read_op& other)
      : base_from_cancellation_state<ReadHandler>(other),
        base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        handler_(other.handler_)
    {
    }

    read_op(read_op&& other)
      : base_from_cancellation_state<ReadHandler>(
          static_cast<base_from_cancellation_state<ReadHandler>&&>(other)),
        base_from_completion_cond<CompletionCondition>(
          static_cast<base_from_completion_cond<CompletionCondition>&&>(other)),
        stream_(other.stream_),
        buffers_(static_cast<buffers_type&&>(other.buffers_)),
        start_(other.start_),
        handler_(static_cast<ReadHandler&&>(other.handler_))
    {
    }

    void operator()(asio::error_code ec,
        std::size_t bytes_transferred, int start = 0)
    {
      std::size_t max_size;
      switch (start_ = start)
      {
        case 1:
        max_size = this->check_for_completion(ec, buffers_.total_consumed());
        for (;;)
        {
          {
            ASIO_HANDLER_LOCATION((__FILE__, __LINE__, "async_read"));
            stream_.async_read_some(buffers_.prepare(max_size),
                static_cast<read_op&&>(*this));
          }
          return; default:
          buffers_.consume(bytes_transferred);
          if ((!ec && bytes_transferred == 0) || buffers_.empty())
            break;
          max_size = this->check_for_completion(ec, buffers_.total_consumed());
          if (max_size == 0)
            break;
          if (this->cancelled() != cancellation_type::none)
          {
            ec = error::operation_aborted;
            break;
          }
        }

        static_cast<ReadHandler&&>(handler_)(
            static_cast<const asio::error_code&>(ec),
            static_cast<const std::size_t&>(buffers_.total_consumed()));
      }
    }

  //private:
    typedef asio::detail::consuming_buffers<mutable_buffer,
        MutableBufferSequence, MutableBufferIterator> buffers_type;

    AsyncReadStream& stream_;
    buffers_type buffers_;
    int start_;
    ReadHandler handler_;
  };

  template <typename AsyncReadStream, typename MutableBufferSequence,
      typename MutableBufferIterator, typename CompletionCondition,
      typename ReadHandler>
  inline bool asio_handler_is_continuation(
      read_op<AsyncReadStream, MutableBufferSequence, MutableBufferIterator,
        CompletionCondition, ReadHandler>* this_handler)
  {
    return this_handler->start_ == 0 ? true
      : asio_handler_cont_helpers::is_continuation(
          this_handler->handler_);
  }

  template <typename AsyncReadStream, typename MutableBufferSequence,
      typename MutableBufferIterator, typename CompletionCondition,
      typename ReadHandler>
  inline void start_read_op(AsyncReadStream& stream,
      const MutableBufferSequence& buffers, const MutableBufferIterator&,
      CompletionCondition& completion_condition, ReadHandler& handler)
  {
    read_op<AsyncReadStream, MutableBufferSequence,
      MutableBufferIterator, CompletionCondition, ReadHandler>(
        stream, buffers, completion_condition, handler)(
          asio::error_code(), 0, 1);
  }

  template <typename AsyncReadStream>
  class initiate_async_read
  {
  public:
    typedef typename AsyncReadStream::executor_type executor_type;

    explicit initiate_async_read(AsyncReadStream& stream)
      : stream_(stream)
    {
    }

    executor_type get_executor() const noexcept
    {
      return stream_.get_executor();
    }

    template <typename ReadHandler, typename MutableBufferSequence,
        typename CompletionCondition>
    void operator()(ReadHandler&& handler,
        const MutableBufferSequence& buffers,
        CompletionCondition&& completion_cond) const
    {
      // If you get an error on the following line it means that your handler
      // does not meet the documented type requirements for a ReadHandler.
      ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

      non_const_lvalue<ReadHandler> handler2(handler);
      non_const_lvalue<CompletionCondition> completion_cond2(completion_cond);
      start_read_op(stream_, buffers,
          asio::buffer_sequence_begin(buffers),
          completion_cond2.value, handler2.value);
    }

  private:
    AsyncReadStream& stream_;
  };
} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

template <template <typename, typename> class Associator,
    typename AsyncReadStream, typename MutableBufferSequence,
    typename MutableBufferIterator, typename CompletionCondition,
    typename ReadHandler, typename DefaultCandidate>
struct associator<Associator,
    detail::read_op<AsyncReadStream, MutableBufferSequence,
      MutableBufferIterator, CompletionCondition, ReadHandler>,
    DefaultCandidate>
  : Associator<ReadHandler, DefaultCandidate>
{
  static typename Associator<ReadHandler, DefaultCandidate>::type get(
      const detail::read_op<AsyncReadStream, MutableBufferSequence,
        MutableBufferIterator, CompletionCondition, ReadHandler>& h) noexcept
  {
    return Associator<ReadHandler, DefaultCandidate>::get(h.handler_);
  }

  static auto get(
      const detail::read_op<AsyncReadStream, MutableBufferSequence,
        MutableBufferIterator, CompletionCondition, ReadHandler>& h,
      const DefaultCandidate& c) noexcept
    -> decltype(Associator<ReadHandler, DefaultCandidate>::get(h.handler_, c))
  {
    return Associator<ReadHandler, DefaultCandidate>::get(h.handler_, c);
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

#if !defined(ASIO_NO_DYNAMIC_BUFFER_V1)

namespace detail
{
  template <typename AsyncReadStream, typename DynamicBuffer_v1,
      typename CompletionCondition, typename ReadHandler>
  class read_dynbuf_v1_op
    : public base_from_cancellation_state<ReadHandler>,
      base_from_completion_cond<CompletionCondition>
  {
  public:
    template <typename BufferSequence>
    read_dynbuf_v1_op(AsyncReadStream& stream,
        BufferSequence&& buffers,
        CompletionCondition& completion_condition, ReadHandler& handler)
      : base_from_cancellation_state<ReadHandler>(
          handler, enable_partial_cancellation()),
        base_from_completion_cond<CompletionCondition>(completion_condition),
        stream_(stream),
        buffers_(static_cast<BufferSequence&&>(buffers)),
        start_(0),
        total_transferred_(0),
        handler_(static_cast<ReadHandler&&>(handler))
    {
    }

    read_dynbuf_v1_op(const read_dynbuf_v1_op& other)
      : base_from_cancellation_state<ReadHandler>(other),
        base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    read_dynbuf_v1_op(read_dynbuf_v1_op&& other)
      : base_from_cancellation_state<ReadHandler>(
          static_cast<base_from_cancellation_state<ReadHandler>&&>(other)),
        base_from_completion_cond<CompletionCondition>(
          static_cast<base_from_completion_cond<CompletionCondition>&&>(other)),
        stream_(other.stream_),
        buffers_(static_cast<DynamicBuffer_v1&&>(other.buffers_)),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(static_cast<ReadHandler&&>(other.handler_))
    {
    }

    void operator()(asio::error_code ec,
        std::size_t bytes_transferred, int start = 0)
    {
      std::size_t max_size, bytes_available;
      switch (start_ = start)
      {
        case 1:
        max_size = this->check_for_completion(ec, total_transferred_);
        bytes_available = std::min<std::size_t>(
              std::max<std::size_t>(512,
                buffers_.capacity() - buffers_.size()),
              std::min<std::size_t>(max_size,
                buffers_.max_size() - buffers_.size()));
        for (;;)
        {
          {
            ASIO_HANDLER_LOCATION((__FILE__, __LINE__, "async_read"));
            stream_.async_read_some(buffers_.prepare(bytes_available),
                static_cast<read_dynbuf_v1_op&&>(*this));
          }
          return; default:
          total_transferred_ += bytes_transferred;
          buffers_.commit(bytes_transferred);
          max_size = this->check_for_completion(ec, total_transferred_);
          bytes_available = std::min<std::size_t>(
                std::max<std::size_t>(512,
                  buffers_.capacity() - buffers_.size()),
                std::min<std::size_t>(max_size,
                  buffers_.max_size() - buffers_.size()));
          if ((!ec && bytes_transferred == 0) || bytes_available == 0)
            break;
          if (this->cancelled() != cancellation_type::none)
          {
            ec = error::operation_aborted;
            break;
          }
        }

        static_cast<ReadHandler&&>(handler_)(
            static_cast<const asio::error_code&>(ec),
            static_cast<const std::size_t&>(total_transferred_));
      }
    }

  //private:
    AsyncReadStream& stream_;
    DynamicBuffer_v1 buffers_;
    int start_;
    std::size_t total_transferred_;
    ReadHandler handler_;
  };

  template <typename AsyncReadStream, typename DynamicBuffer_v1,
      typename CompletionCondition, typename ReadHandler>
  inline bool asio_handler_is_continuation(
      read_dynbuf_v1_op<AsyncReadStream, DynamicBuffer_v1,
        CompletionCondition, ReadHandler>* this_handler)
  {
    return this_handler->start_ == 0 ? true
      : asio_handler_cont_helpers::is_continuation(
          this_handler->handler_);
  }

  template <typename AsyncReadStream>
  class initiate_async_read_dynbuf_v1
  {
  public:
    typedef typename AsyncReadStream::executor_type executor_type;

    explicit initiate_async_read_dynbuf_v1(AsyncReadStream& stream)
      : stream_(stream)
    {
    }

    executor_type get_executor() const noexcept
    {
      return stream_.get_executor();
    }

    template <typename ReadHandler, typename DynamicBuffer_v1,
        typename CompletionCondition>
    void operator()(ReadHandler&& handler,
        DynamicBuffer_v1&& buffers,
        CompletionCondition&& completion_cond) const
    {
      // If you get an error on the following line it means that your handler
      // does not meet the documented type requirements for a ReadHandler.
      ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

      non_const_lvalue<ReadHandler> handler2(handler);
      non_const_lvalue<CompletionCondition> completion_cond2(completion_cond);
      read_dynbuf_v1_op<AsyncReadStream, decay_t<DynamicBuffer_v1>,
        CompletionCondition, decay_t<ReadHandler>>(
          stream_, static_cast<DynamicBuffer_v1&&>(buffers),
            completion_cond2.value, handler2.value)(
              asio::error_code(), 0, 1);
    }

  private:
    AsyncReadStream& stream_;
  };
} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

template <template <typename, typename> class Associator,
    typename AsyncReadStream, typename DynamicBuffer_v1,
    typename CompletionCondition, typename ReadHandler,
    typename DefaultCandidate>
struct associator<Associator,
    detail::read_dynbuf_v1_op<AsyncReadStream,
      DynamicBuffer_v1, CompletionCondition, ReadHandler>,
    DefaultCandidate>
  : Associator<ReadHandler, DefaultCandidate>
{
  static typename Associator<ReadHandler, DefaultCandidate>::type get(
      const detail::read_dynbuf_v1_op<AsyncReadStream, DynamicBuffer_v1,
        CompletionCondition, ReadHandler>& h) noexcept
  {
    return Associator<ReadHandler, DefaultCandidate>::get(h.handler_);
  }

  static auto get(
      const detail::read_dynbuf_v1_op<AsyncReadStream,
        DynamicBuffer_v1, CompletionCondition, ReadHandler>& h,
      const DefaultCandidate& c) noexcept
    -> decltype(Associator<ReadHandler, DefaultCandidate>::get(h.handler_, c))
  {
    return Associator<ReadHandler, DefaultCandidate>::get(h.handler_, c);
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

#endif // !defined(ASIO_NO_DYNAMIC_BUFFER_V1)

namespace detail
{
  template <typename AsyncReadStream, typename DynamicBuffer_v2,
      typename CompletionCondition, typename ReadHandler>
  class read_dynbuf_v2_op
    : public base_from_cancellation_state<ReadHandler>,
      base_from_completion_cond<CompletionCondition>
  {
  public:
    template <typename BufferSequence>
    read_dynbuf_v2_op(AsyncReadStream& stream,
        BufferSequence&& buffers,
        CompletionCondition& completion_condition, ReadHandler& handler)
      : base_from_cancellation_state<ReadHandler>(
          handler, enable_partial_cancellation()),
        base_from_completion_cond<CompletionCondition>(completion_condition),
        stream_(stream),
        buffers_(static_cast<BufferSequence&&>(buffers)),
        start_(0),
        total_transferred_(0),
        bytes_available_(0),
        handler_(static_cast<ReadHandler&&>(handler))
    {
    }

    read_dynbuf_v2_op(const read_dynbuf_v2_op& other)
      : base_from_cancellation_state<ReadHandler>(other),
        base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        bytes_available_(other.bytes_available_),
        handler_(other.handler_)
    {
    }

    read_dynbuf_v2_op(read_dynbuf_v2_op&& other)
      : base_from_cancellation_state<ReadHandler>(
          static_cast<base_from_cancellation_state<ReadHandler>&&>(other)),
        base_from_completion_cond<CompletionCondition>(
          static_cast<base_from_completion_cond<CompletionCondition>&&>(other)),
        stream_(other.stream_),
        buffers_(static_cast<DynamicBuffer_v2&&>(other.buffers_)),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        bytes_available_(other.bytes_available_),
        handler_(static_cast<ReadHandler&&>(other.handler_))
    {
    }

    void operator()(asio::error_code ec,
        std::size_t bytes_transferred, int start = 0)
    {
      std::size_t max_size, pos;
      switch (start_ = start)
      {
        case 1:
        max_size = this->check_for_completion(ec, total_transferred_);
        bytes_available_ = std::min<std::size_t>(
              std::max<std::size_t>(512,
                buffers_.capacity() - buffers_.size()),
              std::min<std::size_t>(max_size,
                buffers_.max_size() - buffers_.size()));
        for (;;)
        {
          pos = buffers_.size();
          buffers_.grow(bytes_available_);
          {
            ASIO_HANDLER_LOCATION((__FILE__, __LINE__, "async_read"));
            stream_.async_read_some(buffers_.data(pos, bytes_available_),
                static_cast<read_dynbuf_v2_op&&>(*this));
          }
          return; default:
          total_transferred_ += bytes_transferred;
          buffers_.shrink(bytes_available_ - bytes_transferred);
          max_size = this->check_for_completion(ec, total_transferred_);
          bytes_available_ = std::min<std::size_t>(
                std::max<std::size_t>(512,
                  buffers_.capacity() - buffers_.size()),
                std::min<std::size_t>(max_size,
                  buffers_.max_size() - buffers_.size()));
          if ((!ec && bytes_transferred == 0) || bytes_available_ == 0)
            break;
          if (this->cancelled() != cancellation_type::none)
          {
            ec = error::operation_aborted;
            break;
          }
        }

        static_cast<ReadHandler&&>(handler_)(
            static_cast<const asio::error_code&>(ec),
            static_cast<const std::size_t&>(total_transferred_));
      }
    }

  //private:
    AsyncReadStream& stream_;
    DynamicBuffer_v2 buffers_;
    int start_;
    std::size_t total_transferred_;
    std::size_t bytes_available_;
    ReadHandler handler_;
  };

  template <typename AsyncReadStream, typename DynamicBuffer_v2,
      typename CompletionCondition, typename ReadHandler>
  inline bool asio_handler_is_continuation(
      read_dynbuf_v2_op<AsyncReadStream, DynamicBuffer_v2,
        CompletionCondition, ReadHandler>* this_handler)
  {
    return this_handler->start_ == 0 ? true
      : asio_handler_cont_helpers::is_continuation(
          this_handler->handler_);
  }

  template <typename AsyncReadStream>
  class initiate_async_read_dynbuf_v2
  {
  public:
    typedef typename AsyncReadStream::executor_type executor_type;

    explicit initiate_async_read_dynbuf_v2(AsyncReadStream& stream)
      : stream_(stream)
    {
    }

    executor_type get_executor() const noexcept
    {
      return stream_.get_executor();
    }

    template <typename ReadHandler, typename DynamicBuffer_v2,
        typename CompletionCondition>
    void operator()(ReadHandler&& handler,
        DynamicBuffer_v2&& buffers,
        CompletionCondition&& completion_cond) const
    {
      // If you get an error on the following line it means that your handler
      // does not meet the documented type requirements for a ReadHandler.
      ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

      non_const_lvalue<ReadHandler> handler2(handler);
      non_const_lvalue<CompletionCondition> completion_cond2(completion_cond);
      read_dynbuf_v2_op<AsyncReadStream, decay_t<DynamicBuffer_v2>,
        CompletionCondition, decay_t<ReadHandler>>(
          stream_, static_cast<DynamicBuffer_v2&&>(buffers),
            completion_cond2.value, handler2.value)(
              asio::error_code(), 0, 1);
    }

  private:
    AsyncReadStream& stream_;
  };
} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

template <template <typename, typename> class Associator,
    typename AsyncReadStream, typename DynamicBuffer_v2,
    typename CompletionCondition, typename ReadHandler,
    typename DefaultCandidate>
struct associator<Associator,
    detail::read_dynbuf_v2_op<AsyncReadStream,
      DynamicBuffer_v2, CompletionCondition, ReadHandler>,
    DefaultCandidate>
  : Associator<ReadHandler, DefaultCandidate>
{
  static typename Associator<ReadHandler, DefaultCandidate>::type get(
      const detail::read_dynbuf_v2_op<AsyncReadStream, DynamicBuffer_v2,
        CompletionCondition, ReadHandler>& h) noexcept
  {
    return Associator<ReadHandler, DefaultCandidate>::get(h.handler_);
  }

  static auto get(
      const detail::read_dynbuf_v2_op<AsyncReadStream,
        DynamicBuffer_v2, CompletionCondition, ReadHandler>& h,
      const DefaultCandidate& c) noexcept
    -> decltype(Associator<ReadHandler, DefaultCandidate>::get(h.handler_, c))
  {
    return Associator<ReadHandler, DefaultCandidate>::get(h.handler_, c);
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_READ_HPP
