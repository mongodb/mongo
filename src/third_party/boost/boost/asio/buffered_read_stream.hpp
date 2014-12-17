//
// buffered_read_stream.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_BUFFERED_READ_STREAM_HPP
#define BOOST_ASIO_BUFFERED_READ_STREAM_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <cstddef>
#include <boost/type_traits/remove_reference.hpp>
#include <boost/asio/buffered_read_stream_fwd.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/detail/bind_handler.hpp>
#include <boost/asio/detail/buffer_resize_guard.hpp>
#include <boost/asio/detail/buffered_stream_storage.hpp>
#include <boost/asio/detail/noncopyable.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_service.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

/// Adds buffering to the read-related operations of a stream.
/**
 * The buffered_read_stream class template can be used to add buffering to the
 * synchronous and asynchronous read operations of a stream.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 *
 * @par Concepts:
 * AsyncReadStream, AsyncWriteStream, Stream, Sync_Read_Stream, SyncWriteStream.
 */
template <typename Stream>
class buffered_read_stream
  : private noncopyable
{
public:
  /// The type of the next layer.
  typedef typename boost::remove_reference<Stream>::type next_layer_type;

  /// The type of the lowest layer.
  typedef typename next_layer_type::lowest_layer_type lowest_layer_type;

#if defined(GENERATING_DOCUMENTATION)
  /// The default buffer size.
  static const std::size_t default_buffer_size = implementation_defined;
#else
  BOOST_STATIC_CONSTANT(std::size_t, default_buffer_size = 1024);
#endif

  /// Construct, passing the specified argument to initialise the next layer.
  template <typename Arg>
  explicit buffered_read_stream(Arg& a)
    : next_layer_(a),
      storage_(default_buffer_size)
  {
  }

  /// Construct, passing the specified argument to initialise the next layer.
  template <typename Arg>
  buffered_read_stream(Arg& a, std::size_t buffer_size)
    : next_layer_(a),
      storage_(buffer_size)
  {
  }

  /// Get a reference to the next layer.
  next_layer_type& next_layer()
  {
    return next_layer_;
  }

  /// Get a reference to the lowest layer.
  lowest_layer_type& lowest_layer()
  {
    return next_layer_.lowest_layer();
  }

  /// Get a const reference to the lowest layer.
  const lowest_layer_type& lowest_layer() const
  {
    return next_layer_.lowest_layer();
  }

  /// Get the io_service associated with the object.
  boost::asio::io_service& get_io_service()
  {
    return next_layer_.get_io_service();
  }

  /// Close the stream.
  void close()
  {
    next_layer_.close();
  }

  /// Close the stream.
  boost::system::error_code close(boost::system::error_code& ec)
  {
    return next_layer_.close(ec);
  }

  /// Write the given data to the stream. Returns the number of bytes written.
  /// Throws an exception on failure.
  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence& buffers)
  {
    return next_layer_.write_some(buffers);
  }

  /// Write the given data to the stream. Returns the number of bytes written,
  /// or 0 if an error occurred.
  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence& buffers,
      boost::system::error_code& ec)
  {
    return next_layer_.write_some(buffers, ec);
  }

  /// Start an asynchronous write. The data being written must be valid for the
  /// lifetime of the asynchronous operation.
  template <typename ConstBufferSequence, typename WriteHandler>
  void async_write_some(const ConstBufferSequence& buffers,
      WriteHandler handler)
  {
    next_layer_.async_write_some(buffers, handler);
  }

  /// Fill the buffer with some data. Returns the number of bytes placed in the
  /// buffer as a result of the operation. Throws an exception on failure.
  std::size_t fill()
  {
    detail::buffer_resize_guard<detail::buffered_stream_storage>
      resize_guard(storage_);
    std::size_t previous_size = storage_.size();
    storage_.resize(storage_.capacity());
    storage_.resize(previous_size + next_layer_.read_some(buffer(
            storage_.data() + previous_size,
            storage_.size() - previous_size)));
    resize_guard.commit();
    return storage_.size() - previous_size;
  }

  /// Fill the buffer with some data. Returns the number of bytes placed in the
  /// buffer as a result of the operation, or 0 if an error occurred.
  std::size_t fill(boost::system::error_code& ec)
  {
    detail::buffer_resize_guard<detail::buffered_stream_storage>
      resize_guard(storage_);
    std::size_t previous_size = storage_.size();
    storage_.resize(storage_.capacity());
    storage_.resize(previous_size + next_layer_.read_some(buffer(
            storage_.data() + previous_size,
            storage_.size() - previous_size),
          ec));
    resize_guard.commit();
    return storage_.size() - previous_size;
  }

  template <typename ReadHandler>
  class fill_handler
  {
  public:
    fill_handler(boost::asio::io_service& io_service,
        detail::buffered_stream_storage& storage,
        std::size_t previous_size, ReadHandler handler)
      : io_service_(io_service),
        storage_(storage),
        previous_size_(previous_size),
        handler_(handler)
    {
    }

    void operator()(const boost::system::error_code& ec,
        std::size_t bytes_transferred)
    {
      storage_.resize(previous_size_ + bytes_transferred);
      io_service_.dispatch(detail::bind_handler(
            handler_, ec, bytes_transferred));
    }

  private:
    boost::asio::io_service& io_service_;
    detail::buffered_stream_storage& storage_;
    std::size_t previous_size_;
    ReadHandler handler_;
  };

  /// Start an asynchronous fill.
  template <typename ReadHandler>
  void async_fill(ReadHandler handler)
  {
    std::size_t previous_size = storage_.size();
    storage_.resize(storage_.capacity());
    next_layer_.async_read_some(
        buffer(
          storage_.data() + previous_size,
          storage_.size() - previous_size),
        fill_handler<ReadHandler>(get_io_service(),
          storage_, previous_size, handler));
  }

  /// Read some data from the stream. Returns the number of bytes read. Throws
  /// an exception on failure.
  template <typename MutableBufferSequence>
  std::size_t read_some(const MutableBufferSequence& buffers)
  {
    if (boost::asio::buffer_size(buffers) == 0)
      return 0;

    if (storage_.empty())
      fill();

    return copy(buffers);
  }

  /// Read some data from the stream. Returns the number of bytes read or 0 if
  /// an error occurred.
  template <typename MutableBufferSequence>
  std::size_t read_some(const MutableBufferSequence& buffers,
      boost::system::error_code& ec)
  {
    ec = boost::system::error_code();

    if (boost::asio::buffer_size(buffers) == 0)
      return 0;

    if (storage_.empty() && !fill(ec))
      return 0;

    return copy(buffers);
  }

  template <typename MutableBufferSequence, typename ReadHandler>
  class read_some_handler
  {
  public:
    read_some_handler(boost::asio::io_service& io_service,
        detail::buffered_stream_storage& storage,
        const MutableBufferSequence& buffers, ReadHandler handler)
      : io_service_(io_service),
        storage_(storage),
        buffers_(buffers),
        handler_(handler)
    {
    }

    void operator()(const boost::system::error_code& ec, std::size_t)
    {
      if (ec || storage_.empty())
      {
        std::size_t length = 0;
        io_service_.dispatch(detail::bind_handler(handler_, ec, length));
      }
      else
      {
        std::size_t bytes_copied = boost::asio::buffer_copy(
            buffers_, storage_.data(), storage_.size());
        storage_.consume(bytes_copied);
        io_service_.dispatch(detail::bind_handler(handler_, ec, bytes_copied));
      }
    }

  private:
    boost::asio::io_service& io_service_;
    detail::buffered_stream_storage& storage_;
    MutableBufferSequence buffers_;
    ReadHandler handler_;
  };

  /// Start an asynchronous read. The buffer into which the data will be read
  /// must be valid for the lifetime of the asynchronous operation.
  template <typename MutableBufferSequence, typename ReadHandler>
  void async_read_some(const MutableBufferSequence& buffers,
      ReadHandler handler)
  {
    if (boost::asio::buffer_size(buffers) == 0)
    {
      get_io_service().post(detail::bind_handler(
            handler, boost::system::error_code(), 0));
    }
    else if (storage_.empty())
    {
      async_fill(read_some_handler<MutableBufferSequence, ReadHandler>(
            get_io_service(), storage_, buffers, handler));
    }
    else
    {
      std::size_t length = copy(buffers);
      get_io_service().post(detail::bind_handler(
            handler, boost::system::error_code(), length));
    }
  }

  /// Peek at the incoming data on the stream. Returns the number of bytes read.
  /// Throws an exception on failure.
  template <typename MutableBufferSequence>
  std::size_t peek(const MutableBufferSequence& buffers)
  {
    if (storage_.empty())
      fill();
    return peek_copy(buffers);
  }

  /// Peek at the incoming data on the stream. Returns the number of bytes read,
  /// or 0 if an error occurred.
  template <typename MutableBufferSequence>
  std::size_t peek(const MutableBufferSequence& buffers,
      boost::system::error_code& ec)
  {
    ec = boost::system::error_code();
    if (storage_.empty() && !fill(ec))
      return 0;
    return peek_copy(buffers);
  }

  /// Determine the amount of data that may be read without blocking.
  std::size_t in_avail()
  {
    return storage_.size();
  }

  /// Determine the amount of data that may be read without blocking.
  std::size_t in_avail(boost::system::error_code& ec)
  {
    ec = boost::system::error_code();
    return storage_.size();
  }

private:
  /// Copy data out of the internal buffer to the specified target buffer.
  /// Returns the number of bytes copied.
  template <typename MutableBufferSequence>
  std::size_t copy(const MutableBufferSequence& buffers)
  {
    std::size_t bytes_copied = boost::asio::buffer_copy(
        buffers, storage_.data(), storage_.size());
    storage_.consume(bytes_copied);
    return bytes_copied;
  }

  /// Copy data from the internal buffer to the specified target buffer, without
  /// removing the data from the internal buffer. Returns the number of bytes
  /// copied.
  template <typename MutableBufferSequence>
  std::size_t peek_copy(const MutableBufferSequence& buffers)
  {
    return boost::asio::buffer_copy(buffers, storage_.data(), storage_.size());
  }

  /// The next layer.
  Stream next_layer_;

  // The data in the buffer.
  detail::buffered_stream_storage storage_;
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_BUFFERED_READ_STREAM_HPP
