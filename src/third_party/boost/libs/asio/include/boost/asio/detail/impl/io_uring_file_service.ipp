//
// detail/impl/io_uring_file_service.ipp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_IMPL_IO_URING_FILE_SERVICE_IPP
#define BOOST_ASIO_DETAIL_IMPL_IO_URING_FILE_SERVICE_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_FILE) \
  && defined(BOOST_ASIO_HAS_IO_URING)

#include <cstring>
#include <sys/stat.h>
#include <boost/asio/detail/io_uring_file_service.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

io_uring_file_service::io_uring_file_service(
    execution_context& context)
  : execution_context_service_base<io_uring_file_service>(context),
    descriptor_service_(context)
{
}

void io_uring_file_service::shutdown()
{
  descriptor_service_.shutdown();
}

boost::system::error_code io_uring_file_service::open(
    io_uring_file_service::implementation_type& impl,
    const char* path, file_base::flags open_flags,
    boost::system::error_code& ec)
{
  if (is_open(impl))
  {
    ec = boost::asio::error::already_open;
    BOOST_ASIO_ERROR_LOCATION(ec);
    return ec;
  }

  descriptor_ops::state_type state = 0;
  int fd = descriptor_ops::open(path, static_cast<int>(open_flags), 0777, ec);
  if (fd < 0)
  {
    BOOST_ASIO_ERROR_LOCATION(ec);
    return ec;
  }

  // We're done. Take ownership of the serial port descriptor.
  if (descriptor_service_.assign(impl, fd, ec))
  {
    boost::system::error_code ignored_ec;
    descriptor_ops::close(fd, state, ignored_ec);
  }

  (void)::posix_fadvise(native_handle(impl), 0, 0,
      impl.is_stream_ ? POSIX_FADV_SEQUENTIAL : POSIX_FADV_RANDOM);

  BOOST_ASIO_ERROR_LOCATION(ec);
  return ec;
}

uint64_t io_uring_file_service::size(
    const io_uring_file_service::implementation_type& impl,
    boost::system::error_code& ec) const
{
  struct stat s;
  int result = ::fstat(native_handle(impl), &s);
  descriptor_ops::get_last_error(ec, result != 0);
  BOOST_ASIO_ERROR_LOCATION(ec);
  return !ec ? s.st_size : 0;
}

boost::system::error_code io_uring_file_service::resize(
    io_uring_file_service::implementation_type& impl,
    uint64_t n, boost::system::error_code& ec)
{
  int result = ::ftruncate(native_handle(impl), n);
  descriptor_ops::get_last_error(ec, result != 0);
  BOOST_ASIO_ERROR_LOCATION(ec);
  return ec;
}

boost::system::error_code io_uring_file_service::sync_all(
    io_uring_file_service::implementation_type& impl,
    boost::system::error_code& ec)
{
  int result = ::fsync(native_handle(impl));
  descriptor_ops::get_last_error(ec, result != 0);
  return ec;
}

boost::system::error_code io_uring_file_service::sync_data(
    io_uring_file_service::implementation_type& impl,
    boost::system::error_code& ec)
{
#if defined(_POSIX_SYNCHRONIZED_IO)
  int result = ::fdatasync(native_handle(impl));
#else // defined(_POSIX_SYNCHRONIZED_IO)
  int result = ::fsync(native_handle(impl));
#endif // defined(_POSIX_SYNCHRONIZED_IO)
  descriptor_ops::get_last_error(ec, result != 0);
  BOOST_ASIO_ERROR_LOCATION(ec);
  return ec;
}

uint64_t io_uring_file_service::seek(
    io_uring_file_service::implementation_type& impl, int64_t offset,
    file_base::seek_basis whence, boost::system::error_code& ec)
{
  int64_t result = ::lseek(native_handle(impl), offset, whence);
  descriptor_ops::get_last_error(ec, result < 0);
  BOOST_ASIO_ERROR_LOCATION(ec);
  return !ec ? static_cast<uint64_t>(result) : 0;
}

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // defined(BOOST_ASIO_HAS_FILE)
       //   && defined(BOOST_ASIO_HAS_IO_URING)

#endif // BOOST_ASIO_DETAIL_IMPL_IO_URING_FILE_SERVICE_IPP
