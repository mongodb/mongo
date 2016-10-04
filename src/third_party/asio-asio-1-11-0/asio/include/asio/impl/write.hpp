//
// impl/write.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_WRITE_HPP
#define ASIO_IMPL_WRITE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

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

#include "asio/detail/push_options.hpp"

namespace asio {

template <typename SyncWriteStream, typename ConstBufferSequence,
    typename CompletionCondition>
std::size_t write(SyncWriteStream& s, const ConstBufferSequence& buffers,
    CompletionCondition completion_condition, asio::error_code& ec,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type*)
{
  ec = asio::error_code();
  asio::detail::consuming_buffers<
    const_buffer, ConstBufferSequence> tmp(buffers);
  std::size_t total_transferred = 0;
  tmp.prepare(detail::adapt_completion_condition_result(
        completion_condition(ec, total_transferred)));
  while (tmp.begin() != tmp.end())
  {
    std::size_t bytes_transferred = s.write_some(tmp, ec);
    tmp.consume(bytes_transferred);
    total_transferred += bytes_transferred;
    tmp.prepare(detail::adapt_completion_condition_result(
          completion_condition(ec, total_transferred)));
  }
  return total_transferred;
}

template <typename SyncWriteStream, typename ConstBufferSequence>
inline std::size_t write(SyncWriteStream& s, const ConstBufferSequence& buffers,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type*)
{
  asio::error_code ec;
  std::size_t bytes_transferred = write(s, buffers, transfer_all(), ec);
  asio::detail::throw_error(ec, "write");
  return bytes_transferred;
}

template <typename SyncWriteStream, typename ConstBufferSequence>
inline std::size_t write(SyncWriteStream& s, const ConstBufferSequence& buffers,
    asio::error_code& ec,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type*)
{
  return write(s, buffers, transfer_all(), ec);
}

template <typename SyncWriteStream, typename ConstBufferSequence,
    typename CompletionCondition>
inline std::size_t write(SyncWriteStream& s, const ConstBufferSequence& buffers,
    CompletionCondition completion_condition,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type*)
{
  asio::error_code ec;
  std::size_t bytes_transferred = write(s, buffers, completion_condition, ec);
  asio::detail::throw_error(ec, "write");
  return bytes_transferred;
}

template <typename SyncWriteStream, typename DynamicBufferSequence,
    typename CompletionCondition>
std::size_t write(SyncWriteStream& s,
    ASIO_MOVE_ARG(DynamicBufferSequence) buffers,
    CompletionCondition completion_condition, asio::error_code& ec,
    typename enable_if<
      is_dynamic_buffer_sequence<DynamicBufferSequence>::value
    >::type*)
{
  typename decay<DynamicBufferSequence>::type b(
      ASIO_MOVE_CAST(DynamicBufferSequence)(buffers));

  std::size_t bytes_transferred = write(s, b.data(), completion_condition, ec);
  b.consume(bytes_transferred);
  return bytes_transferred;
}

template <typename SyncWriteStream, typename DynamicBufferSequence>
inline std::size_t write(SyncWriteStream& s,
    ASIO_MOVE_ARG(DynamicBufferSequence) buffers,
    typename enable_if<
      is_dynamic_buffer_sequence<DynamicBufferSequence>::value
    >::type*)
{
  asio::error_code ec;
  std::size_t bytes_transferred = write(s,
      ASIO_MOVE_CAST(DynamicBufferSequence)(buffers),
      transfer_all(), ec);
  asio::detail::throw_error(ec, "write");
  return bytes_transferred;
}

template <typename SyncWriteStream, typename DynamicBufferSequence>
inline std::size_t write(SyncWriteStream& s,
    ASIO_MOVE_ARG(DynamicBufferSequence) buffers,
    asio::error_code& ec,
    typename enable_if<
      is_dynamic_buffer_sequence<DynamicBufferSequence>::value
    >::type*)
{
  return write(s, ASIO_MOVE_CAST(DynamicBufferSequence)(buffers),
      transfer_all(), ec);
}

template <typename SyncWriteStream, typename DynamicBufferSequence,
    typename CompletionCondition>
inline std::size_t write(SyncWriteStream& s,
    ASIO_MOVE_ARG(DynamicBufferSequence) buffers,
    CompletionCondition completion_condition,
    typename enable_if<
      is_dynamic_buffer_sequence<DynamicBufferSequence>::value
    >::type*)
{
  asio::error_code ec;
  std::size_t bytes_transferred = write(s,
      ASIO_MOVE_CAST(DynamicBufferSequence)(buffers),
      completion_condition, ec);
  asio::detail::throw_error(ec, "write");
  return bytes_transferred;
}

#if !defined(ASIO_NO_IOSTREAM)

template <typename SyncWriteStream, typename Allocator,
    typename CompletionCondition>
inline std::size_t write(SyncWriteStream& s,
    asio::basic_streambuf<Allocator>& b,
    CompletionCondition completion_condition, asio::error_code& ec)
{
  return write(s, basic_streambuf_ref<Allocator>(b), completion_condition, ec);
}

template <typename SyncWriteStream, typename Allocator>
inline std::size_t write(SyncWriteStream& s,
    asio::basic_streambuf<Allocator>& b)
{
  return write(s, basic_streambuf_ref<Allocator>(b));
}

template <typename SyncWriteStream, typename Allocator>
inline std::size_t write(SyncWriteStream& s,
    asio::basic_streambuf<Allocator>& b,
    asio::error_code& ec)
{
  return write(s, basic_streambuf_ref<Allocator>(b), ec);
}

template <typename SyncWriteStream, typename Allocator,
    typename CompletionCondition>
inline std::size_t write(SyncWriteStream& s,
    asio::basic_streambuf<Allocator>& b,
    CompletionCondition completion_condition)
{
  return write(s, basic_streambuf_ref<Allocator>(b), completion_condition);
}

#endif // !defined(ASIO_NO_IOSTREAM)

namespace detail
{
  template <typename AsyncWriteStream, typename ConstBufferSequence,
      typename CompletionCondition, typename WriteHandler>
  class write_op
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    write_op(AsyncWriteStream& stream, const ConstBufferSequence& buffers,
        CompletionCondition completion_condition, WriteHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        stream_(stream),
        buffers_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(WriteHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    write_op(const write_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    write_op(write_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(WriteHandler)(other.handler_))
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
          stream_.async_write_some(buffers_,
              ASIO_MOVE_CAST(write_op)(*this));
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
    AsyncWriteStream& stream_;
    asio::detail::consuming_buffers<
      const_buffer, ConstBufferSequence> buffers_;
    int start_;
    std::size_t total_transferred_;
    WriteHandler handler_;
  };

  template <typename AsyncWriteStream,
      typename CompletionCondition, typename WriteHandler>
  class write_op<AsyncWriteStream, asio::mutable_buffers_1,
      CompletionCondition, WriteHandler>
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    write_op(AsyncWriteStream& stream,
        const asio::mutable_buffers_1& buffers,
        CompletionCondition completion_condition,
        WriteHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        stream_(stream),
        buffer_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(WriteHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    write_op(const write_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffer_(other.buffer_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    write_op(write_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffer_(other.buffer_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(WriteHandler)(other.handler_))
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
          stream_.async_write_some(
              asio::buffer(buffer_ + total_transferred_, n),
              ASIO_MOVE_CAST(write_op)(*this));
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
    AsyncWriteStream& stream_;
    asio::mutable_buffer buffer_;
    int start_;
    std::size_t total_transferred_;
    WriteHandler handler_;
  };

  template <typename AsyncWriteStream,
      typename CompletionCondition, typename WriteHandler>
  class write_op<AsyncWriteStream, asio::const_buffers_1,
      CompletionCondition, WriteHandler>
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    write_op(AsyncWriteStream& stream,
        const asio::const_buffers_1& buffers,
        CompletionCondition completion_condition,
        WriteHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        stream_(stream),
        buffer_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(WriteHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    write_op(const write_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffer_(other.buffer_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    write_op(write_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffer_(other.buffer_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(WriteHandler)(other.handler_))
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
          stream_.async_write_some(
              asio::buffer(buffer_ + total_transferred_, n),
              ASIO_MOVE_CAST(write_op)(*this));
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
    AsyncWriteStream& stream_;
    asio::const_buffer buffer_;
    int start_;
    std::size_t total_transferred_;
    WriteHandler handler_;
  };

  template <typename AsyncWriteStream, typename Elem,
      typename CompletionCondition, typename WriteHandler>
  class write_op<AsyncWriteStream, boost::array<Elem, 2>,
      CompletionCondition, WriteHandler>
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    write_op(AsyncWriteStream& stream, const boost::array<Elem, 2>& buffers,
        CompletionCondition completion_condition, WriteHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        stream_(stream),
        buffers_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(WriteHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    write_op(const write_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    write_op(write_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(WriteHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred, int start = 0)
    {
      typename asio::detail::dependent_type<Elem,
          boost::array<asio::const_buffer, 2> >::type bufs = {{
        asio::const_buffer(buffers_[0]),
        asio::const_buffer(buffers_[1]) }};
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
          stream_.async_write_some(bufs, ASIO_MOVE_CAST(write_op)(*this));
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
    AsyncWriteStream& stream_;
    boost::array<Elem, 2> buffers_;
    int start_;
    std::size_t total_transferred_;
    WriteHandler handler_;
  };

#if defined(ASIO_HAS_STD_ARRAY)

  template <typename AsyncWriteStream, typename Elem,
      typename CompletionCondition, typename WriteHandler>
  class write_op<AsyncWriteStream, std::array<Elem, 2>,
      CompletionCondition, WriteHandler>
    : detail::base_from_completion_cond<CompletionCondition>
  {
  public:
    write_op(AsyncWriteStream& stream, const std::array<Elem, 2>& buffers,
        CompletionCondition completion_condition, WriteHandler& handler)
      : detail::base_from_completion_cond<
          CompletionCondition>(completion_condition),
        stream_(stream),
        buffers_(buffers),
        start_(0),
        total_transferred_(0),
        handler_(ASIO_MOVE_CAST(WriteHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    write_op(const write_op& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(other.handler_)
    {
    }

    write_op(write_op&& other)
      : detail::base_from_completion_cond<CompletionCondition>(other),
        stream_(other.stream_),
        buffers_(other.buffers_),
        start_(other.start_),
        total_transferred_(other.total_transferred_),
        handler_(ASIO_MOVE_CAST(WriteHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred, int start = 0)
    {
      typename asio::detail::dependent_type<Elem,
          std::array<asio::const_buffer, 2> >::type bufs = {{
        asio::const_buffer(buffers_[0]),
        asio::const_buffer(buffers_[1]) }};
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
          stream_.async_write_some(bufs, ASIO_MOVE_CAST(write_op)(*this));
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
    AsyncWriteStream& stream_;
    std::array<Elem, 2> buffers_;
    int start_;
    std::size_t total_transferred_;
    WriteHandler handler_;
  };

#endif // defined(ASIO_HAS_STD_ARRAY)

  template <typename AsyncWriteStream, typename ConstBufferSequence,
      typename CompletionCondition, typename WriteHandler>
  inline void* asio_handler_allocate(std::size_t size,
      write_op<AsyncWriteStream, ConstBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    return asio_handler_alloc_helpers::allocate(
        size, this_handler->handler_);
  }

  template <typename AsyncWriteStream, typename ConstBufferSequence,
      typename CompletionCondition, typename WriteHandler>
  inline void asio_handler_deallocate(void* pointer, std::size_t size,
      write_op<AsyncWriteStream, ConstBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    asio_handler_alloc_helpers::deallocate(
        pointer, size, this_handler->handler_);
  }

  template <typename AsyncWriteStream, typename ConstBufferSequence,
      typename CompletionCondition, typename WriteHandler>
  inline bool asio_handler_is_continuation(
      write_op<AsyncWriteStream, ConstBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    return this_handler->start_ == 0 ? true
      : asio_handler_cont_helpers::is_continuation(
          this_handler->handler_);
  }

  template <typename Function, typename AsyncWriteStream,
      typename ConstBufferSequence, typename CompletionCondition,
      typename WriteHandler>
  inline void asio_handler_invoke(Function& function,
      write_op<AsyncWriteStream, ConstBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }

  template <typename Function, typename AsyncWriteStream,
      typename ConstBufferSequence, typename CompletionCondition,
      typename WriteHandler>
  inline void asio_handler_invoke(const Function& function,
      write_op<AsyncWriteStream, ConstBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }
} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

template <typename AsyncWriteStream, typename ConstBufferSequence,
    typename CompletionCondition, typename WriteHandler, typename Allocator>
struct associated_allocator<
    detail::write_op<AsyncWriteStream, ConstBufferSequence,
      CompletionCondition, WriteHandler>,
    Allocator>
{
  typedef typename associated_allocator<WriteHandler, Allocator>::type type;

  static type get(
      const detail::write_op<AsyncWriteStream, ConstBufferSequence,
        CompletionCondition, WriteHandler>& h,
      const Allocator& a = Allocator()) ASIO_NOEXCEPT
  {
    return associated_allocator<WriteHandler, Allocator>::get(h.handler_, a);
  }
};

template <typename AsyncWriteStream, typename ConstBufferSequence,
    typename CompletionCondition, typename WriteHandler, typename Executor>
struct associated_executor<
    detail::write_op<AsyncWriteStream, ConstBufferSequence,
      CompletionCondition, WriteHandler>,
    Executor>
{
  typedef typename associated_executor<WriteHandler, Executor>::type type;

  static type get(
      const detail::write_op<AsyncWriteStream, ConstBufferSequence,
        CompletionCondition, WriteHandler>& h,
      const Executor& ex = Executor()) ASIO_NOEXCEPT
  {
    return associated_executor<WriteHandler, Executor>::get(h.handler_, ex);
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

template <typename AsyncWriteStream, typename ConstBufferSequence,
  typename CompletionCondition, typename WriteHandler>
inline ASIO_INITFN_RESULT_TYPE(WriteHandler,
    void (asio::error_code, std::size_t))
async_write(AsyncWriteStream& s, const ConstBufferSequence& buffers,
    CompletionCondition completion_condition,
    ASIO_MOVE_ARG(WriteHandler) handler,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type*)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a WriteHandler.
  ASIO_WRITE_HANDLER_CHECK(WriteHandler, handler) type_check;

  async_completion<WriteHandler,
    void (asio::error_code, std::size_t)> init(handler);

  detail::write_op<AsyncWriteStream, ConstBufferSequence,
    CompletionCondition, ASIO_HANDLER_TYPE(
      WriteHandler, void (asio::error_code, std::size_t))>(
        s, buffers, completion_condition, init.handler)(
          asio::error_code(), 0, 1);

  return init.result.get();
}

template <typename AsyncWriteStream, typename ConstBufferSequence,
    typename WriteHandler>
inline ASIO_INITFN_RESULT_TYPE(WriteHandler,
    void (asio::error_code, std::size_t))
async_write(AsyncWriteStream& s, const ConstBufferSequence& buffers,
    ASIO_MOVE_ARG(WriteHandler) handler,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type*)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a WriteHandler.
  ASIO_WRITE_HANDLER_CHECK(WriteHandler, handler) type_check;

  async_completion<WriteHandler,
    void (asio::error_code, std::size_t)> init(handler);

  detail::write_op<AsyncWriteStream, ConstBufferSequence,
    detail::transfer_all_t, ASIO_HANDLER_TYPE(
      WriteHandler, void (asio::error_code, std::size_t))>(
        s, buffers, transfer_all(), init.handler)(
          asio::error_code(), 0, 1);

  return init.result.get();
}

namespace detail
{
  template <typename AsyncWriteStream, typename DynamicBufferSequence,
      typename CompletionCondition, typename WriteHandler>
  class write_dynbuf_op
  {
  public:
    template <typename BufferSequence>
    write_dynbuf_op(AsyncWriteStream& stream,
        ASIO_MOVE_ARG(BufferSequence) buffers,
        CompletionCondition completion_condition, WriteHandler& handler)
      : stream_(stream),
        buffers_(ASIO_MOVE_CAST(BufferSequence)(buffers)),
        completion_condition_(
          ASIO_MOVE_CAST(CompletionCondition)(completion_condition)),
        handler_(ASIO_MOVE_CAST(WriteHandler)(handler))
    {
    }

#if defined(ASIO_HAS_MOVE)
    write_dynbuf_op(const write_dynbuf_op& other)
      : stream_(other.stream_),
        buffers_(other.buffers_),
        completion_condition_(other.completion_condition_),
        handler_(other.handler_)
    {
    }

    write_dynbuf_op(write_dynbuf_op&& other)
      : stream_(other.stream_),
        buffers_(ASIO_MOVE_CAST(DynamicBufferSequence)(other.buffers_)),
        completion_condition_(
          ASIO_MOVE_CAST(CompletionCondition)(
            other.completion_condition_)),
        handler_(ASIO_MOVE_CAST(WriteHandler)(other.handler_))
    {
    }
#endif // defined(ASIO_HAS_MOVE)

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred, int start = 0)
    {
      switch (start)
      {
        case 1:
        async_write(stream_, buffers_.data(), completion_condition_,
            ASIO_MOVE_CAST(write_dynbuf_op)(*this));
        return; default:
        buffers_.consume(bytes_transferred);
        handler_(ec, static_cast<const std::size_t&>(bytes_transferred));
      }
    }

  //private:
    AsyncWriteStream& stream_;
    DynamicBufferSequence buffers_;
    CompletionCondition completion_condition_;
    WriteHandler handler_;
  };

  template <typename AsyncWriteStream, typename DynamicBufferSequence,
      typename CompletionCondition, typename WriteHandler>
  inline void* asio_handler_allocate(std::size_t size,
      write_dynbuf_op<AsyncWriteStream, DynamicBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    return asio_handler_alloc_helpers::allocate(
        size, this_handler->handler_);
  }

  template <typename AsyncWriteStream, typename DynamicBufferSequence,
      typename CompletionCondition, typename WriteHandler>
  inline void asio_handler_deallocate(void* pointer, std::size_t size,
      write_dynbuf_op<AsyncWriteStream, DynamicBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    asio_handler_alloc_helpers::deallocate(
        pointer, size, this_handler->handler_);
  }

  template <typename AsyncWriteStream, typename DynamicBufferSequence,
      typename CompletionCondition, typename WriteHandler>
  inline bool asio_handler_is_continuation(
      write_dynbuf_op<AsyncWriteStream, DynamicBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    return asio_handler_cont_helpers::is_continuation(
        this_handler->handler_);
  }

  template <typename Function, typename AsyncWriteStream,
      typename DynamicBufferSequence, typename CompletionCondition,
      typename WriteHandler>
  inline void asio_handler_invoke(Function& function,
      write_dynbuf_op<AsyncWriteStream, DynamicBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }

  template <typename Function, typename AsyncWriteStream,
      typename DynamicBufferSequence, typename CompletionCondition,
      typename WriteHandler>
  inline void asio_handler_invoke(const Function& function,
      write_dynbuf_op<AsyncWriteStream, DynamicBufferSequence,
        CompletionCondition, WriteHandler>* this_handler)
  {
    asio_handler_invoke_helpers::invoke(
        function, this_handler->handler_);
  }
} // namespace detail

#if !defined(GENERATING_DOCUMENTATION)

template <typename AsyncWriteStream, typename DynamicBufferSequence,
    typename CompletionCondition, typename WriteHandler, typename Allocator>
struct associated_allocator<
    detail::write_dynbuf_op<AsyncWriteStream,
      DynamicBufferSequence, CompletionCondition, WriteHandler>,
    Allocator>
{
  typedef typename associated_allocator<WriteHandler, Allocator>::type type;

  static type get(
      const detail::write_dynbuf_op<AsyncWriteStream,
        DynamicBufferSequence, CompletionCondition, WriteHandler>& h,
      const Allocator& a = Allocator()) ASIO_NOEXCEPT
  {
    return associated_allocator<WriteHandler, Allocator>::get(h.handler_, a);
  }
};

template <typename AsyncWriteStream, typename DynamicBufferSequence,
    typename CompletionCondition, typename WriteHandler, typename Executor>
struct associated_executor<
    detail::write_dynbuf_op<AsyncWriteStream,
      DynamicBufferSequence, CompletionCondition, WriteHandler>,
    Executor>
{
  typedef typename associated_executor<WriteHandler, Executor>::type type;

  static type get(
      const detail::write_dynbuf_op<AsyncWriteStream,
        DynamicBufferSequence, CompletionCondition, WriteHandler>& h,
      const Executor& ex = Executor()) ASIO_NOEXCEPT
  {
    return associated_executor<WriteHandler, Executor>::get(h.handler_, ex);
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

template <typename AsyncWriteStream,
    typename DynamicBufferSequence, typename WriteHandler>
inline ASIO_INITFN_RESULT_TYPE(WriteHandler,
    void (asio::error_code, std::size_t))
async_write(AsyncWriteStream& s,
    ASIO_MOVE_ARG(DynamicBufferSequence) buffers,
    ASIO_MOVE_ARG(WriteHandler) handler,
    typename enable_if<
      is_dynamic_buffer_sequence<DynamicBufferSequence>::value
    >::type*)
{
  return async_write(s,
      ASIO_MOVE_CAST(DynamicBufferSequence)(buffers),
      transfer_all(), ASIO_MOVE_CAST(WriteHandler)(handler));
}

template <typename AsyncWriteStream, typename DynamicBufferSequence,
    typename CompletionCondition, typename WriteHandler>
inline ASIO_INITFN_RESULT_TYPE(WriteHandler,
    void (asio::error_code, std::size_t))
async_write(AsyncWriteStream& s,
    ASIO_MOVE_ARG(DynamicBufferSequence) buffers,
    CompletionCondition completion_condition,
    ASIO_MOVE_ARG(WriteHandler) handler,
    typename enable_if<
      is_dynamic_buffer_sequence<DynamicBufferSequence>::value
    >::type*)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a WriteHandler.
  ASIO_WRITE_HANDLER_CHECK(WriteHandler, handler) type_check;

  async_completion<WriteHandler,
    void (asio::error_code, std::size_t)> init(handler);

  detail::write_dynbuf_op<AsyncWriteStream,
    typename decay<DynamicBufferSequence>::type,
      CompletionCondition, ASIO_HANDLER_TYPE(
        WriteHandler, void (asio::error_code, std::size_t))>(
          s, ASIO_MOVE_CAST(DynamicBufferSequence)(buffers),
            completion_condition, init.handler)(
              asio::error_code(), 0, 1);

  return init.result.get();
}

#if !defined(ASIO_NO_IOSTREAM)

template <typename AsyncWriteStream, typename Allocator, typename WriteHandler>
inline ASIO_INITFN_RESULT_TYPE(WriteHandler,
    void (asio::error_code, std::size_t))
async_write(AsyncWriteStream& s,
    asio::basic_streambuf<Allocator>& b,
    ASIO_MOVE_ARG(WriteHandler) handler)
{
  return async_write(s, basic_streambuf_ref<Allocator>(b),
      ASIO_MOVE_CAST(WriteHandler)(handler));
}

template <typename AsyncWriteStream, typename Allocator,
    typename CompletionCondition, typename WriteHandler>
inline ASIO_INITFN_RESULT_TYPE(WriteHandler,
    void (asio::error_code, std::size_t))
async_write(AsyncWriteStream& s,
    asio::basic_streambuf<Allocator>& b,
    CompletionCondition completion_condition,
    ASIO_MOVE_ARG(WriteHandler) handler)
{
  return async_write(s, basic_streambuf_ref<Allocator>(b),
      completion_condition, ASIO_MOVE_CAST(WriteHandler)(handler));
}

#endif // !defined(ASIO_NO_IOSTREAM)

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_WRITE_HPP
