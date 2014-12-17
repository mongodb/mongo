//
// buffered_write_stream.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_BUFFERED_WRITE_STREAM_HPP
#define BOOST_ASIO_BUFFERED_WRITE_STREAM_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <cstddef>
#include <boost/type_traits/remove_reference.hpp>
#include <boost/asio/buffered_write_stream_fwd.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/completion_condition.hpp>
#include <boost/asio/detail/bind_handler.hpp>
#include <boost/asio/detail/buffered_stream_storage.hpp>
#include <boost/asio/detail/noncopyable.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/write.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

/// Adds buffering to the write-related operations of a stream.
/**
 * The buffered_write_stream class template can be used to add buffering to the
 * synchronous and asynchronous write operations of a stream.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 *
 * @par Concepts:
 * AsyncReadStream, AsyncWriteStream, Stream, SyncReadStream, SyncWriteStream.
 */
template <typename Stream>
class buffered_write_stream
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
  explicit buffered_write_stream(Arg& a)
    : next_layer_(a),
      storage_(default_buffer_size)
  {
  }

  /// Construct, passing the specified argument to initialise the next layer.
  template <typename Arg>
  buffered_write_stream(Arg& a, std::size_t buffer_size)
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

  /// Flush all data from the buffer to the next layer. Returns the number of
  /// bytes written to the next layer on the last write operation. Throws an
  /// exception on failure.
  std::size_t flush()
  {
    std::size_t bytes_written = write(next_layer_,
        buffer(storage_.data(), storage_.size()));
    storage_.consume(bytes_written);
    return bytes_written;
  }

  /// Flush all data from the buffer to the next layer. Returns the number of
  /// bytes written to the next layer on the last write operation, or 0 if an
  /// error occurred.
  std::size_t flush(boost::system::error_code& ec)
  {
    std::size_t bytes_written = write(next_layer_,
        buffer(storage_.data(), storage_.size()),
        transfer_all(), ec);
    storage_.consume(bytes_written);
    return bytes_written;
  }

  template <typename WriteHandler>
  class flush_handler
  {
  public:
    flush_handler(boost::asio::io_service& io_service,
        detail::buffered_stream_storage& storage, WriteHandler handler)
      : io_service_(io_service),
        storage_(storage),
        handler_(handler)
    {
    }

    void operator()(const boost::system::error_code& ec,
        std::size_t bytes_written)
    {
      storage_.consume(bytes_written);
      io_service_.dispatch(detail::bind_handler(handler_, ec, bytes_written));
    }

  private:
    boost::asio::io_service& io_service_;
    detail::buffered_stream_storage& storage_;
    WriteHandler handler_;
  };

  /// Start an asynchronous flush.
  template <typename WriteHandler>
  void async_flush(WriteHandler handler)
  {
    async_write(next_layer_, buffer(storage_.data(), storage_.size()),
        flush_handler<WriteHandler>(get_io_service(), storage_, handler));
  }

  /// Write the given data to the stream. Returns the number of bytes written.
  /// Throws an exception on failure.
  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence& buffers)
  {
    if (boost::asio::buffer_size(buffers) == 0)
      return 0;

    if (storage_.size() == storage_.capacity())
      flush();

    return copy(buffers);
  }

  /// Write the given data to the stream. Returns the number of bytes written,
  /// or 0 if an error occurred and the error handler did not throw.
  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence& buffers,
      boost::system::error_code& ec)
  {
    ec = boost::system::error_code();

    if (boost::asio::buffer_size(buffers) == 0)
      return 0;

    if (storage_.size() == storage_.capacity() && !flush(ec))
      return 0;

    return copy(buffers);
  }

  template <typename ConstBufferSequence, typename WriteHandler>
  class write_some_handler
  {
  public:
    write_some_handler(boost::asio::io_service& io_service,
        detail::buffered_stream_storage& storage,
        const ConstBufferSequence& buffers, WriteHandler handler)
      : io_service_(io_service),
        storage_(storage),
        buffers_(buffers),
        handler_(handler)
    {
    }

    void operator()(const boost::system::error_code& ec, std::size_t)
    {
      if (ec)
      {
        std::size_t length = 0;
        io_service_.dispatch(detail::bind_handler(handler_, ec, length));
      }
      else
      {
        std::size_t orig_size = storage_.size();
        std::size_t space_avail = storage_.capacity() - orig_size;
        std::size_t bytes_avail = boost::asio::buffer_size(buffers_);
        std::size_t length = bytes_avail < space_avail
          ? bytes_avail : space_avail;
        storage_.resize(orig_size + length);
        std::size_t bytes_copied = boost::asio::buffer_copy(
            storage_.data(), buffers_, length);

        io_service_.dispatch(detail::bind_handler(handler_, ec, bytes_copied));
      }
    }

  private:
    boost::asio::io_service& io_service_;
    detail::buffered_stream_storage& storage_;
    ConstBufferSequence buffers_;
    WriteHandler handler_;
  };

  /// Start an asynchronous write. The data being written must be valid for the
  /// lifetime of the asynchronous operation.
  template <typename ConstBufferSequence, typename WriteHandler>
  void async_write_some(const ConstBufferSequence& buffers,
      WriteHandler handler)
  {
    if (boost::asio::buffer_size(buffers) == 0)
    {
      get_io_service().post(detail::bind_handler(
            handler, boost::system::error_code(), 0));
    }
    else if (storage_.size() == storage_.capacity())
    {
      async_flush(write_some_handler<ConstBufferSequence, WriteHandler>(
            get_io_service(), storage_, buffers, handler));
    }
    else
    {
      std::size_t bytes_copied = copy(buffers);
      get_io_service().post(detail::bind_handler(
            handler, boost::system::error_code(), bytes_copied));
    }
  }

  /// Read some data from the stream. Returns the number of bytes read. Throws
  /// an exception on failure.
  template <typename MutableBufferSequence>
  std::size_t read_some(const MutableBufferSequence& buffers)
  {
    return next_layer_.read_some(buffers);
  }

  /// Read some data from the stream. Returns the number of bytes read or 0 if
  /// an error occurred.
  template <typename MutableBufferSequence>
  std::size_t read_some(const MutableBufferSequence& buffers,
      boost::system::error_code& ec)
  {
    return next_layer_.read_some(buffers, ec);
  }

  /// Start an asynchronous read. The buffer into which the data will be read
  /// must be valid for the lifetime of the asynchronous operation.
  template <typename MutableBufferSequence, typename ReadHandler>
  void async_read_some(const MutableBufferSequence& buffers,
      ReadHandler handler)
  {
    next_layer_.async_read_some(buffers, handler);
  }

  /// Peek at the incoming data on the stream. Returns the number of bytes read.
  /// Throws an exception on failure.
  template <typename MutableBufferSequence>
  std::size_t peek(const MutableBufferSequence& buffers)
  {
    return next_layer_.peek(buffers);
  }

  /// Peek at the incoming data on the stream. Returns the number of bytes read,
  /// or 0 if an error occurred.
  template <typename MutableBufferSequence>
  std::size_t peek(const MutableBufferSequence& buffers,
      boost::system::error_code& ec)
  {
    return next_layer_.peek(buffers, ec);
  }

  /// Determine the amount of data that may be read without blocking.
  std::size_t in_avail()
  {
    return next_layer_.in_avail();
  }

  /// Determine the amount of data that may be read without blocking.
  std::size_t in_avail(boost::system::error_code& ec)
  {
    return next_layer_.in_avail(ec);
  }

private:
  /// Copy data into the internal buffer from the specified source buffer.
  /// Returns the number of bytes copied.
  template <typename ConstBufferSequence>
  std::size_t copy(const ConstBufferSequence& buffers)
  {
    std::size_t orig_size = storage_.size();
    std::size_t space_avail = storage_.capacity() - orig_size;
    std::size_t bytes_avail = boost::asio::buffer_size(buffers);
    std::size_t length = bytes_avail < space_avail ? bytes_avail : space_avail;
    storage_.resize(orig_size + length);
    return boost::asio::buffer_copy(
        storage_.data() + orig_size, buffers, length);
  }

  /// The next layer.
  Stream next_layer_;

  // The data in the buffer.
  detail::buffered_stream_storage storage_;
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_BUFFERED_WRITE_STREAM_HPP
