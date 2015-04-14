//
// detail/buffer_sequence_adapter.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_BUFFER_SEQUENCE_ADAPTER_HPP
#define ASIO_DETAIL_BUFFER_SEQUENCE_ADAPTER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/buffer.hpp"
#include "asio/detail/array_fwd.hpp"
#include "asio/detail/socket_types.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

class buffer_sequence_adapter_base
{
protected:
#if defined(ASIO_WINDOWS_RUNTIME)
  // The maximum number of buffers to support in a single operation.
  enum { max_buffers = 1 };

  typedef Windows::Storage::Streams::IBuffer^ native_buffer_type;

  ASIO_DECL static void init_native_buffer(
      native_buffer_type& buf,
      const asio::mutable_buffer& buffer);

  ASIO_DECL static void init_native_buffer(
      native_buffer_type& buf,
      const asio::const_buffer& buffer);
#elif defined(ASIO_WINDOWS) || defined(__CYGWIN__)
  // The maximum number of buffers to support in a single operation.
  enum { max_buffers = 64 < max_iov_len ? 64 : max_iov_len };

  typedef WSABUF native_buffer_type;

  static void init_native_buffer(WSABUF& buf,
      const asio::mutable_buffer& buffer)
  {
    buf.buf = asio::buffer_cast<char*>(buffer);
    buf.len = static_cast<ULONG>(asio::buffer_size(buffer));
  }

  static void init_native_buffer(WSABUF& buf,
      const asio::const_buffer& buffer)
  {
    buf.buf = const_cast<char*>(asio::buffer_cast<const char*>(buffer));
    buf.len = static_cast<ULONG>(asio::buffer_size(buffer));
  }
#else // defined(ASIO_WINDOWS) || defined(__CYGWIN__)
  // The maximum number of buffers to support in a single operation.
  enum { max_buffers = 64 < max_iov_len ? 64 : max_iov_len };

  typedef iovec native_buffer_type;

  static void init_iov_base(void*& base, void* addr)
  {
    base = addr;
  }

  template <typename T>
  static void init_iov_base(T& base, void* addr)
  {
    base = static_cast<T>(addr);
  }

  static void init_native_buffer(iovec& iov,
      const asio::mutable_buffer& buffer)
  {
    init_iov_base(iov.iov_base, asio::buffer_cast<void*>(buffer));
    iov.iov_len = asio::buffer_size(buffer);
  }

  static void init_native_buffer(iovec& iov,
      const asio::const_buffer& buffer)
  {
    init_iov_base(iov.iov_base, const_cast<void*>(
          asio::buffer_cast<const void*>(buffer)));
    iov.iov_len = asio::buffer_size(buffer);
  }
#endif // defined(ASIO_WINDOWS) || defined(__CYGWIN__)
};

// Helper class to translate buffers into the native buffer representation.
template <typename Buffer, typename Buffers>
class buffer_sequence_adapter
  : buffer_sequence_adapter_base
{
public:
  explicit buffer_sequence_adapter(const Buffers& buffer_sequence)
    : count_(0), total_buffer_size_(0)
  {
    typename Buffers::const_iterator iter = buffer_sequence.begin();
    typename Buffers::const_iterator end = buffer_sequence.end();
    for (; iter != end && count_ < max_buffers; ++iter, ++count_)
    {
      Buffer buffer(*iter);
      init_native_buffer(buffers_[count_], buffer);
      total_buffer_size_ += asio::buffer_size(buffer);
    }
  }

  native_buffer_type* buffers()
  {
    return buffers_;
  }

  std::size_t count() const
  {
    return count_;
  }

  bool all_empty() const
  {
    return total_buffer_size_ == 0;
  }

  static bool all_empty(const Buffers& buffer_sequence)
  {
    typename Buffers::const_iterator iter = buffer_sequence.begin();
    typename Buffers::const_iterator end = buffer_sequence.end();
    std::size_t i = 0;
    for (; iter != end && i < max_buffers; ++iter, ++i)
      if (asio::buffer_size(Buffer(*iter)) > 0)
        return false;
    return true;
  }

  static void validate(const Buffers& buffer_sequence)
  {
    typename Buffers::const_iterator iter = buffer_sequence.begin();
    typename Buffers::const_iterator end = buffer_sequence.end();
    for (; iter != end; ++iter)
    {
      Buffer buffer(*iter);
      asio::buffer_cast<const void*>(buffer);
    }
  }

  static Buffer first(const Buffers& buffer_sequence)
  {
    typename Buffers::const_iterator iter = buffer_sequence.begin();
    typename Buffers::const_iterator end = buffer_sequence.end();
    for (; iter != end; ++iter)
    {
      Buffer buffer(*iter);
      if (asio::buffer_size(buffer) != 0)
        return buffer;
    }
    return Buffer();
  }

private:
  native_buffer_type buffers_[max_buffers];
  std::size_t count_;
  std::size_t total_buffer_size_;
};

template <typename Buffer>
class buffer_sequence_adapter<Buffer, asio::mutable_buffers_1>
  : buffer_sequence_adapter_base
{
public:
  explicit buffer_sequence_adapter(
      const asio::mutable_buffers_1& buffer_sequence)
  {
    init_native_buffer(buffer_, Buffer(buffer_sequence));
    total_buffer_size_ = asio::buffer_size(buffer_sequence);
  }

  native_buffer_type* buffers()
  {
    return &buffer_;
  }

  std::size_t count() const
  {
    return 1;
  }

  bool all_empty() const
  {
    return total_buffer_size_ == 0;
  }

  static bool all_empty(const asio::mutable_buffers_1& buffer_sequence)
  {
    return asio::buffer_size(buffer_sequence) == 0;
  }

  static void validate(const asio::mutable_buffers_1& buffer_sequence)
  {
    asio::buffer_cast<const void*>(buffer_sequence);
  }

  static Buffer first(const asio::mutable_buffers_1& buffer_sequence)
  {
    return Buffer(buffer_sequence);
  }

private:
  native_buffer_type buffer_;
  std::size_t total_buffer_size_;
};

template <typename Buffer>
class buffer_sequence_adapter<Buffer, asio::const_buffers_1>
  : buffer_sequence_adapter_base
{
public:
  explicit buffer_sequence_adapter(
      const asio::const_buffers_1& buffer_sequence)
  {
    init_native_buffer(buffer_, Buffer(buffer_sequence));
    total_buffer_size_ = asio::buffer_size(buffer_sequence);
  }

  native_buffer_type* buffers()
  {
    return &buffer_;
  }

  std::size_t count() const
  {
    return 1;
  }

  bool all_empty() const
  {
    return total_buffer_size_ == 0;
  }

  static bool all_empty(const asio::const_buffers_1& buffer_sequence)
  {
    return asio::buffer_size(buffer_sequence) == 0;
  }

  static void validate(const asio::const_buffers_1& buffer_sequence)
  {
    asio::buffer_cast<const void*>(buffer_sequence);
  }

  static Buffer first(const asio::const_buffers_1& buffer_sequence)
  {
    return Buffer(buffer_sequence);
  }

private:
  native_buffer_type buffer_;
  std::size_t total_buffer_size_;
};

template <typename Buffer, typename Elem>
class buffer_sequence_adapter<Buffer, boost::array<Elem, 2> >
  : buffer_sequence_adapter_base
{
public:
  explicit buffer_sequence_adapter(
      const boost::array<Elem, 2>& buffer_sequence)
  {
    init_native_buffer(buffers_[0], Buffer(buffer_sequence[0]));
    init_native_buffer(buffers_[1], Buffer(buffer_sequence[1]));
    total_buffer_size_ = asio::buffer_size(buffer_sequence[0])
      + asio::buffer_size(buffer_sequence[1]);
  }

  native_buffer_type* buffers()
  {
    return buffers_;
  }

  std::size_t count() const
  {
    return 2;
  }

  bool all_empty() const
  {
    return total_buffer_size_ == 0;
  }

  static bool all_empty(const boost::array<Elem, 2>& buffer_sequence)
  {
    return asio::buffer_size(buffer_sequence[0]) == 0
      && asio::buffer_size(buffer_sequence[1]) == 0;
  }

  static void validate(const boost::array<Elem, 2>& buffer_sequence)
  {
    asio::buffer_cast<const void*>(buffer_sequence[0]);
    asio::buffer_cast<const void*>(buffer_sequence[1]);
  }

  static Buffer first(const boost::array<Elem, 2>& buffer_sequence)
  {
    return Buffer(asio::buffer_size(buffer_sequence[0]) != 0
        ? buffer_sequence[0] : buffer_sequence[1]);
  }

private:
  native_buffer_type buffers_[2];
  std::size_t total_buffer_size_;
};

#if defined(ASIO_HAS_STD_ARRAY)

template <typename Buffer, typename Elem>
class buffer_sequence_adapter<Buffer, std::array<Elem, 2> >
  : buffer_sequence_adapter_base
{
public:
  explicit buffer_sequence_adapter(
      const std::array<Elem, 2>& buffer_sequence)
  {
    init_native_buffer(buffers_[0], Buffer(buffer_sequence[0]));
    init_native_buffer(buffers_[1], Buffer(buffer_sequence[1]));
    total_buffer_size_ = asio::buffer_size(buffer_sequence[0])
      + asio::buffer_size(buffer_sequence[1]);
  }

  native_buffer_type* buffers()
  {
    return buffers_;
  }

  std::size_t count() const
  {
    return 2;
  }

  bool all_empty() const
  {
    return total_buffer_size_ == 0;
  }

  static bool all_empty(const std::array<Elem, 2>& buffer_sequence)
  {
    return asio::buffer_size(buffer_sequence[0]) == 0
      && asio::buffer_size(buffer_sequence[1]) == 0;
  }

  static void validate(const std::array<Elem, 2>& buffer_sequence)
  {
    asio::buffer_cast<const void*>(buffer_sequence[0]);
    asio::buffer_cast<const void*>(buffer_sequence[1]);
  }

  static Buffer first(const std::array<Elem, 2>& buffer_sequence)
  {
    return Buffer(asio::buffer_size(buffer_sequence[0]) != 0
        ? buffer_sequence[0] : buffer_sequence[1]);
  }

private:
  native_buffer_type buffers_[2];
  std::size_t total_buffer_size_;
};

#endif // defined(ASIO_HAS_STD_ARRAY)

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#if defined(ASIO_HEADER_ONLY)
# include "asio/detail/impl/buffer_sequence_adapter.ipp"
#endif // defined(ASIO_HEADER_ONLY)

#endif // ASIO_DETAIL_BUFFER_SEQUENCE_ADAPTER_HPP
