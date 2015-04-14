//
// impl/read_at.hpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_READ_AT_HPP
#define ASIO_IMPL_READ_AT_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <algorithm>
#include "asio/associated_allocator.hpp"
#include "asio/associated_executor.hpp"
#include "asio/buffer.hpp"
#include "asio/completion_condition.hpp"
#include "asio/detail/array_fwd.hpp"
#include "asio/detail/base_from_completion_cond.hpp"
#include "asio/detail/bind_handler.hpp"
#include "asio/detail/consuming_buffers.hpp"
#include "asio/detail/dependent_type.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_cont_helpers.hpp"
#include "asio/detail/handler_invoke_helpers.hpp"
#include "asio/detail/handler_type_requirements.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

template <typename SyncRandomAccessReadDevice, typename MutableBufferSequence,
    typename CompletionCondition>
std::size_t read_at(SyncRandomAccessReadDevice& d,
    uint64_t offset, const MutableBufferSequence& buffers,
    CompletionCondition completion_condition, asio::error_code& ec)
{
  ec = asio::error_code();
  asio::detail::consuming_buffers<
    mutable_buffer, MutableBufferSequence> tmp(buffers);
  std::size_t total_transferred = 0;
  tmp.prepare(detail::adapt_completion_condition_result(
        completion_condition(ec, total_transferred)));
  while (tmp.begin() != tmp.end())
  {
    std::size_t bytes_transferred = d.read_some_at(
        offset + total_transferred, tmp, ec);
    tmp.consume(bytes_transferred);
    total_transferred += bytes_transferred;
    tmp.prepare(detail::adapt_completion_condition_result(
          completion_condition(ec, total_transferred)));
  }
  return total_transferred;
}

template <typename SyncRandomAccessReadDevice, typename MutableBufferSequence>
inline std::size_t read_at(SyncRandomAccessReadDevice& d,
    uint64_t offset, const MutableBufferSequence& buffers)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read_at(
      d, offset, buffers, transfer_all(), ec);
  asio::detail::throw_error(ec, "read_at");
  return bytes_transferred;
}

template <typename SyncRandomAccessReadDevice, typename MutableBufferSequence>
inline std::size_t read_at(SyncRandomAccessReadDevice& d,
    uint64_t offset, const MutableBufferSequence& buffers,
    asio::error_code& ec)
{
  return read_at(d, offset, buffers, transfer_all(), ec);
}

template <typename SyncRandomAccessReadDevice, typename MutableBufferSequence,
    typename CompletionCondition>
inline std::size_t read_at(SyncRandomAccessReadDevice& d,
    uint64_t offset, const MutableBufferSequence& buffers,
    CompletionCondition completion_condition)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read_at(
      d, offset, buffers, completion_condition, ec);
  asio::detail::throw_error(ec, "read_at");
  return bytes_transferred;
}

#if !defined(ASIO_NO_IOSTREAM)

template <typename SyncRandomAccessReadDevice, typename Allocator,
    typename CompletionCondition>
std::size_t read_at(SyncRandomAccessReadDevice& d,
    uint64_t offset, asio::basic_streambuf<Allocator>& b,
    CompletionCondition completion_condition, asio::error_code& ec)
{
  ec = asio::error_code();
  std::size_t total_transferred = 0;
  std::size_t max_size = detail::adapt_completion_condition_result(
        completion_condition(ec, total_transferred));
  std::size_t bytes_available = read_size_helper(b, max_size);
  while (bytes_available > 0)
  {
    std::size_t bytes_transferred = d.read_some_at(
        offset + total_transferred, b.prepare(bytes_available), ec);
    b.commit(bytes_transferred);
    total_transferred += bytes_transferred;
    max_size = detail::adapt_completion_condition_result(
          completion_condition(ec, total_transferred));
    bytes_available = read_size_helper(b, max_size);
  }
  return total_transferred;
}

template <typename SyncRandomAccessReadDevice, typename Allocator>
inline std::size_t read_at(SyncRandomAccessReadDevice& d,
    uint64_t offset, asio::basic_streambuf<Allocator>& b)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read_at(
      d, offset, b, transfer_all(), ec);
  asio::detail::throw_error(ec, "read_at");
  return bytes_transferred;
}

template <typename SyncRandomAccessReadDevice, typename Allocator>
inline std::size_t read_at(SyncRandomAccessReadDevice& d,
    uint64_t offset, asio::basic_streambuf<Allocator>& b,
    asio::error_code& ec)
{
  return read_at(d, offset, b, transfer_all(), ec);
}

template <typename SyncRandomAccessReadDevice, typename Allocator,
    typename CompletionCondition>
inline std::size_t read_at(SyncRandomAccessReadDevice& d,
    uint64_t offset, asio::basic_streambuf<Allocator>& b,
    CompletionCondition completion_condition)
{
  asio::error_code ec;
  std::size_t bytes_transferred = read_at(
      d, offset, b, completion_condition, ec);
  asio::detail::throw_error(ec, "read_at");
  return bytes_transferred;
}

#endif // !defined(ASIO_NO_IOSTREAM)

namespace detail
{
  template <typename AsyncRandomAccessReadDevice,
      typename MutableBufferSequence, typename CompletionCondition,
      typename ReadHandler>
  class read_at_op
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    read_at_op(AsyncRandomAccessReadDevice& device,
        uint64_t offset, const MutableBufferSequence& buffers,
        CompletionCondition completion_condition, ReadHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        device_(device),
        offset_(offset),
        buffers_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(ReadHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    read_at_op(const read_at_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    read_at_op(read_at_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(ReadHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred, int start = 0)
    {
      switch (start_ = start)
      {
        case 1:
        buffers_.prepare(this->check_for_completion(ec, total_transferred_));
        for (;;)
        {
          device_.async_read_some_at(offset_ + total_transferred_,
              buffers_, ASIO_MOVE_CAST(read_at_op)(*this));
          return; default:
          total_transferred_ += bytes_transferred;
          buffers_.consume(bytes_transferred);
          buffers_.prepare(this->check_for_completion(ec, total_transferred_));
          if ((!ec && bytes_transferred == 0)
              || buffers_.begin() == buffers_.end())
            break;
        }

        handler_(ec, static_cast<const std::size_t&>(total_transferred_));
      }
    }

  //private:
    AsyncRandomAccessReadDevice& device_;
    uint64_t offset_;
    asio::detail::consuming_buffers<
      mutable_buffer, MutableBufferSequence> buffers_;
    int start_;
    std::size_t total_transferred_;
    ReadHandler handler_;
  };

  template <typename AsyncRandomAccessReadDevice,
      typename CompletionCondition, typename ReadHandler>
  class read_at_op<AsyncRandomAccessReadDevice,
      asio::mutable_buffers_1, CompletionCondition, ReadHandler>
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    read_at_op(AsyncRandomAccessReadDevice& device,
        uint64_t offset, const asio::mutable_buffers_1& buffers,
        CompletionCondition completion_condition, ReadHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        device_(device),
        offset_(offset),
        buffer_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(ReadHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    read_at_op(const read_at_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        buffer_(other.buffer_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    read_at_op(read_at_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        buffer_(other.buffer_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(ReadHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred, int start = 0)
    {
      std::size_t n = 0;
      switch (start_ = start)
      {
        case 1:
        n = this->check_for_completion(ec, total_transferred_);
        for (;;)
        {
          device_.async_read_some_at(offset_ + total_transferred_,
              asio::buffer(buffer_ + total_transferred_, n),
              ASIO_MOVE_CAST(read_at_op)(*this));
          return; default:
          total_transferred_ += bytes_transferred;
          if ((!ec && bytes_transferred == 0)
              || (n = this->check_for_completion(ec, total_transferred_)) == 0
              || total_transferred_ == asio::buffer_size(buffer_))
            break;
        }

        handler_(ec, static_cast<const std::size_t&>(total_transferred_));
      }
    }

  //private:
    AsyncRandomAccessReadDevice& device_;
    uint64_t offset_;
    asio::mutable_buffer buffer_;
    int start_;
    std::size_t total_transferred_;
    ReadHandler handler_;
  };

  template <typename AsyncRandomAccessReadDevice, typename Elem,
      typename CompletionCondition, typename ReadHandler>
  class read_at_op<AsyncRandomAccessReadDevice, boost::array<Elem, 2>,
      CompletionCondition, ReadHandler>
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    read_at_op(AsyncRandomAccessReadDevice& device,
        uint64_t offset, const boost::array<Elem, 2>& buffers,
        CompletionCondition completion_condition, ReadHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        device_(device),
        offset_(offset),
        buffers_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(ReadHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    read_at_op(const read_at_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    read_at_op(read_at_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(ReadHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred, int start = 0)
    {
      typename asio::detail::dependent_type<Elem,
          boost::array<asio::mutable_buffer, 2> >::type bufs = {{
        asio::mutable_buffer(buffers_[0]),
        asio::mutable_buffer(buffers_[1]) }};
      std::size_t buffer_size0 = asio::buffer_size(bufs[0]);
      std::size_t buffer_size1 = asio::buffer_size(bufs[1]);
      std::size_t n = 0;
      switch (start_ = start)
      {
        case 1:
        n = this->check_for_completion(ec, total_transferred_);
        for (;;)
        {
          bufs[0] = asio::buffer(bufs[0] + total_transferred_, n);
          bufs[1] = asio::buffer(
              bufs[1] + (total_transferred_ < buffer_size0
                ? 0 : total_transferred_ - buffer_size0),
              n - asio::buffer_size(bufs[0]));
          device_.async_read_some_at(offset_ + total_transferred_,
              bufs, ASIO_MOVE_CAST(read_at_op)(*this));
          return; default:
          total_transferred_ += bytes_transferred;
          if ((!ec && bytes_transferred == 0)
              || (n = this->check_for_completion(ec, total_transferred_)) == 0
              || total_transferred_ == buffer_size0 + buffer_size1)
            break;
        }

        handler_(ec, static_cast<const std::size_t&>(total_transferred_));
      }
    }

  //private:
    AsyncRandomAccessReadDevice& device_;
    uint64_t offset_;
    boost::array<Elem, 2> buffers_;
    int start_;
    std::size_t total_transferred_;
    ReadHandler handler_;
  };

#if defined(ASIO_HAS_STD_ARRAY)

  template <typename AsyncRandomAccessReadDevice, typename Elem,
      typename CompletionCondition, typename ReadHandler>
  class read_at_op<AsyncRandomAccessReadDevice, std::array<Elem, 2>,
      CompletionCondition, ReadHandler>
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    read_at_op(AsyncRandomAccessReadDevice& device,
        uint64_t offset, const std::array<Elem, 2>& buffers,
        CompletionCondition completion_condition, ReadHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        device_(device),
        offset_(offset),
        buffers_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(ReadHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    read_at_op(const read_at_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    read_at_op(read_at_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(ReadHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred, int start = 0)
    {
      typename asio::detail::dependent_type<Elem,
          std::array<asio::mutable_buffer, 2> >::type bufs = {{
        asio::mutable_buffer(buffers_[0]),
        asio::mutable_buffer(buffers_[1]) }};
      std::size_t buffer_size0 = asio::buffer_size(bufs[0]);
      std::size_t buffer_size1 = asio::buffer_size(bufs[1]);
      std::size_t n = 0;
      switch (start_ = start)
      {
        case 1:
        n = this->check_for_completion(ec, total_transferred_);
        for (;;)
        {
          bufs[0] = asio::buffer(bufs[0] + total_transferred_, n);
          bufs[1] = asio::buffer(
              bufs[1] + (total_transferred_ < buffer_size0
                ? 0 : total_transferred_ - buffer_size0),
              n - asio::buffer_size(bufs[0]));
          device_.async_read_some_at(offset_ + total_transferred_,
              bufs, ASIO_MOVE_CAST(read_at_op)(*this));
          return; default:
          total_transferred_ += bytes_transferred;
          if ((!ec && bytes_transferred == 0)
              || (n = this->check_for_completion(ec, total_transferred_)) == 0
              || total_transferred_ == buffer_size0 + buffer_size1)
            break;
        }

        handler_(ec, static_cast<const std::size_t&>(total_transferred_));
      }
    }

  //private:
    AsyncRandomAccessReadDevice& device_;
    uint64_t offset_;
    std::array<Elem, 2> buffers_;
    int start_;
    std::size_t total_transferred_;
    ReadHandler handler_;
  };

#endif // defined(ASIO_HAS_STD_ARRAY)

  template <typename AsyncRandomAccessReadDevice,
      typename MutableBufferSequence, typename CompletionCondition,
      typename ReadHandler>
  inline void* asio_handler_allocate(std::size_t size,
      read_at_op<AsyncRandomAccessReadDevice, MutableBufferSequence,
        CompletionCondition, ReadHandler>* this_handler)
  {
    return asio_handler_alloc_helpers::allocate(
        size, this_handler->handler_);
  }

  template <typename AsyncRandomAccessReadDevice,
      typename MutableBufferSequence, typename CompletionCondition,
      typename ReadHandler>
  inline void asio_handler_deallocate(void* pointer, std::size_t size,
      read_at_op<AsyncRandomAccessReadDevice, MutableBufferSequence,
        CompletionCondition, ReadHandler>* this_handler)
  {
    asio_handler_alloc_helpers::deallocate(
        pointer, size, this_handler->handler_);
  }

  template <typename AsyncRandomAccessReadDevice,
      typename MutableBufferSequence, typename CompletionCondition,
      typename ReadHandler>
  inline bool asio_handler_is_continuation(
      read_at_op<AsyncRandomAccessReadDevice, MutableBufferSequence,
        CompletionCondition, ReadHandler>* this_handler)
  {
    return this_handler->start_ == 0 ? true
      : asio_handler_cont_helpers::is_continuation(
          this_handler->handler_);
  }

  template <typename Function, typename AsyncRandomAccessReadDevice,
      typename MutableBufferSequence, typename CompletionCondition,
      typename ReadHandler>
  inline void asio_handler_invoke(Function& function,
      read_at_op<AsyncRandomAccessReadDevice, MutableBufferSequence,
        CompletionCondition, ReadHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }

  template <typename Function, typename AsyncRandomAccessReadDevice,
      typename MutableBufferSequence, typename CompletionCondition,
      typename ReadHandler>
  inline void asio_handler_invoke(const Function& function,
      read_at_op<AsyncRandomAccessReadDevice, MutableBufferSequence,
        CompletionCondition, ReadHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }

  template <typename AsyncRandomAccessReadDevice,
      typename MutableBufferSequence, typename CompletionCondition,
      typename ReadHandler>
  inline read_at_op<AsyncRandomAccessReadDevice,
      MutableBufferSequence, CompletionCondition, ReadHandler>
  make_read_at_op(AsyncRandomAccessReadDevice& d,
      uint64_t offset, const MutableBufferSequence& buffers,
      CompletionCondition completion_condition, ReadHandler handler)
  {
    return read_at_op<AsyncRandomAccessReadDevice,
      MutableBufferSequence, CompletionCondition, ReadHandler>(
        d, offset, buffers, completion_condition, handler);
  }
} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

template <typename AsyncRandomAccessReadDevice, typename MutableBufferSequence,
    typename CompletionCondition, typename ReadHandler, typename Allocator>
struct associated_allocator<
    detail::read_at_op<AsyncRandomAccessReadDevice,
      MutableBufferSequence, CompletionCondition, ReadHandler>,
    Allocator>
{
  typedef typename associated_allocator<ReadHandler, Allocator>::type type;

  static type get(
      const detail::read_at_op<AsyncRandomAccessReadDevice,
        MutableBufferSequence, CompletionCondition, ReadHandler>& h,
      const Allocator& a = Allocator()) ASIO_NOEXCEPT
  {
    return associated_allocator<ReadHandler, Allocator>::get(h.handler_, a);
  }
};

template <typename AsyncRandomAccessReadDevice, typename MutableBufferSequence,
    typename CompletionCondition, typename ReadHandler, typename Executor>
struct associated_executor<
    detail::read_at_op<AsyncRandomAccessReadDevice,
      MutableBufferSequence, CompletionCondition, ReadHandler>,
    Executor>
{
  typedef typename associated_executor<ReadHandler, Executor>::type type;

  static type get(
      const detail::read_at_op<AsyncRandomAccessReadDevice,
        MutableBufferSequence, CompletionCondition, ReadHandler>& h,
      const Executor& ex = Executor()) ASIO_NOEXCEPT
  {
    return associated_executor<ReadHandler, Executor>::get(h.handler_, ex);
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

template <typename AsyncRandomAccessReadDevice, typename MutableBufferSequence,
    typename CompletionCondition, typename ReadHandler>
inline ASIO_INITFN_RESULT_TYPE(ReadHandler,
    void (asio::error_code, std::size_t))
async_read_at(AsyncRandomAccessReadDevice& d,
    uint64_t offset, const MutableBufferSequence& buffers,
    CompletionCondition completion_condition,
    ASIO_MOVE_ARG(ReadHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a ReadHandler.
  ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

  async_completion<ReadHandler,
    void (asio::error_code, std::size_t)> init(handler);

  detail::read_at_op<AsyncRandomAccessReadDevice, MutableBufferSequence,
    CompletionCondition, ASIO_HANDLER_TYPE(ReadHandler,
      void (asio::error_code, std::size_t))>(
        d, offset, buffers, completion_condition, init.handler)(
          asio::error_code(), 0, 1);

  return init.result.get();
}

template <typename AsyncRandomAccessReadDevice, typename MutableBufferSequence,
    typename ReadHandler>
inline ASIO_INITFN_RESULT_TYPE(ReadHandler,
    void (asio::error_code, std::size_t))
async_read_at(AsyncRandomAccessReadDevice& d,
    uint64_t offset, const MutableBufferSequence& buffers,
    ASIO_MOVE_ARG(ReadHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a ReadHandler.
  ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

  async_completion<ReadHandler,
    void (asio::error_code, std::size_t)> init(handler);

  detail::read_at_op<AsyncRandomAccessReadDevice, MutableBufferSequence,
    detail::transfer_all_t, ASIO_HANDLER_TYPE(ReadHandler,
      void (asio::error_code, std::size_t))>(
        d, offset, buffers, transfer_all(), init.handler)(
          asio::error_code(), 0, 1);

  return init.result.get();
}

#if !defined(ASIO_NO_IOSTREAM)

namespace detail
{
  template <typename AsyncRandomAccessReadDevice, typename Allocator,
      typename CompletionCondition, typename ReadHandler>
  class read_at_streambuf_op
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    read_at_streambuf_op(AsyncRandomAccessReadDevice& device,
        uint64_t offset, basic_streambuf<Allocator>& streambuf,
        CompletionCondition completion_condition, ReadHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        device_(device),
        offset_(offset),
        streambuf_(streambuf),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(ReadHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    read_at_streambuf_op(const read_at_streambuf_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        streambuf_(other.streambuf_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    read_at_streambuf_op(read_at_streambuf_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        device_(other.device_),
        offset_(other.offset_),
        streambuf_(other.streambuf_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(ReadHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred, int start = 0)
    {
      std::size_t max_size, bytes_available;
      switch (start_ = start)
      {
        case 1:
        max_size = this->check_for_completion(ec, total_transferred_);
        bytes_available = read_size_helper(streambuf_, max_size);
        for (;;)
        {
          device_.async_read_some_at(offset_ + total_transferred_,
              streambuf_.prepare(bytes_available),
              ASIO_MOVE_CAST(read_at_streambuf_op)(*this));
          return; default:
          total_transferred_ += bytes_transferred;
          streambuf_.commit(bytes_transferred);
          max_size = this->check_for_completion(ec, total_transferred_);
          bytes_available = read_size_helper(streambuf_, max_size);
          if ((!ec && bytes_transferred == 0) || bytes_available == 0)
            break;
        }

        handler_(ec, static_cast<const std::size_t&>(total_transferred_));
      }
    }

  //private:
    AsyncRandomAccessReadDevice& device_;
    uint64_t offset_;
    asio::basic_streambuf<Allocator>& streambuf_;
    int start_;
    std::size_t total_transferred_;
    ReadHandler handler_;
  };

  template <typename AsyncRandomAccessReadDevice, typename Allocator,
      typename CompletionCondition, typename ReadHandler>
  inline void* asio_handler_allocate(std::size_t size,
      read_at_streambuf_op<AsyncRandomAccessReadDevice, Allocator,
        CompletionCondition, ReadHandler>* this_handler)
  {
    return asio_handler_alloc_helpers::allocate(
        size, this_handler->handler_);
  }

  template <typename AsyncRandomAccessReadDevice, typename Allocator,
      typename CompletionCondition, typename ReadHandler>
  inline void asio_handler_deallocate(void* pointer, std::size_t size,
      read_at_streambuf_op<AsyncRandomAccessReadDevice, Allocator,
        CompletionCondition, ReadHandler>* this_handler)
  {
    asio_handler_alloc_helpers::deallocate(
        pointer, size, this_handler->handler_);
  }

  template <typename AsyncRandomAccessReadDevice, typename Allocator,
      typename CompletionCondition, typename ReadHandler>
  inline bool asio_handler_is_continuation(
      read_at_streambuf_op<AsyncRandomAccessReadDevice, Allocator,
        CompletionCondition, ReadHandler>* this_handler)
  {
    return this_handler->start_ == 0 ? true
      : asio_handler_cont_helpers::is_continuation(
          this_handler->handler_);
  }

  template <typename Function, typename AsyncRandomAccessReadDevice,
      typename Allocator, typename CompletionCondition, typename ReadHandler>
  inline void asio_handler_invoke(Function& function,
      read_at_streambuf_op<AsyncRandomAccessReadDevice, Allocator,
        CompletionCondition, ReadHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }

  template <typename Function, typename AsyncRandomAccessReadDevice,
      typename Allocator, typename CompletionCondition, typename ReadHandler>
  inline void asio_handler_invoke(const Function& function,
      read_at_streambuf_op<AsyncRandomAccessReadDevice, Allocator,
        CompletionCondition, ReadHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }
} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

template <typename AsyncRandomAccessReadDevice, typename Allocator,
    typename CompletionCondition, typename ReadHandler, typename Allocator1>
struct associated_allocator<
    detail::read_at_streambuf_op<AsyncRandomAccessReadDevice,
      Allocator, CompletionCondition, ReadHandler>,
    Allocator1>
{
  typedef typename associated_allocator<ReadHandler, Allocator1>::type type;

  static type get(
      const detail::read_at_streambuf_op<AsyncRandomAccessReadDevice,
        Allocator, CompletionCondition, ReadHandler>& h,
      const Allocator1& a = Allocator1()) ASIO_NOEXCEPT
  {
    return associated_allocator<ReadHandler, Allocator1>::get(h.handler_, a);
  }
};

template <typename AsyncRandomAccessReadDevice, typename Executor,
    typename CompletionCondition, typename ReadHandler, typename Executor1>
struct associated_executor<
    detail::read_at_streambuf_op<AsyncRandomAccessReadDevice,
      Executor, CompletionCondition, ReadHandler>,
    Executor1>
{
  typedef typename associated_executor<ReadHandler, Executor1>::type type;

  static type get(
      const detail::read_at_streambuf_op<AsyncRandomAccessReadDevice,
        Executor, CompletionCondition, ReadHandler>& h,
      const Executor1& ex = Executor1()) ASIO_NOEXCEPT
  {
    return associated_executor<ReadHandler, Executor1>::get(h.handler_, ex);
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

template <typename AsyncRandomAccessReadDevice, typename Allocator,
    typename CompletionCondition, typename ReadHandler>
inline ASIO_INITFN_RESULT_TYPE(ReadHandler,
    void (asio::error_code, std::size_t))
async_read_at(AsyncRandomAccessReadDevice& d,
    uint64_t offset, asio::basic_streambuf<Allocator>& b,
    CompletionCondition completion_condition,
    ASIO_MOVE_ARG(ReadHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a ReadHandler.
  ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

  async_completion<ReadHandler,
    void (asio::error_code, std::size_t)> init(handler);

  detail::read_at_streambuf_op<AsyncRandomAccessReadDevice, Allocator,
    CompletionCondition, ASIO_HANDLER_TYPE(ReadHandler,
      void (asio::error_code, std::size_t))>(
        d, offset, b, completion_condition, init.handler)(
          asio::error_code(), 0, 1);

  return init.result.get();
}

template <typename AsyncRandomAccessReadDevice, typename Allocator,
    typename ReadHandler>
inline ASIO_INITFN_RESULT_TYPE(ReadHandler,
    void (asio::error_code, std::size_t))
async_read_at(AsyncRandomAccessReadDevice& d,
    uint64_t offset, asio::basic_streambuf<Allocator>& b,
    ASIO_MOVE_ARG(ReadHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a ReadHandler.
  ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

  async_completion<ReadHandler,
    void (asio::error_code, std::size_t)> init(handler);

  detail::read_at_streambuf_op<AsyncRandomAccessReadDevice, Allocator,
    detail::transfer_all_t, ASIO_HANDLER_TYPE(ReadHandler,
      void (asio::error_code, std::size_t))>(
        d, offset, b, transfer_all(), init.handler)(
          asio::error_code(), 0, 1);

  return init.result.get();
}

#endif // !defined(ASIO_NO_IOSTREAM)

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_READ_AT_HPP
