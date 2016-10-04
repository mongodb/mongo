//
// impl/io_service.ipp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_IO_SERVICE_IPP
#define ASIO_IMPL_IO_SERVICE_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/io_service.hpp"
#include "asio/detail/limits.hpp"
#include "asio/detail/scoped_ptr.hpp"
#include "asio/detail/service_registry.hpp"
#include "asio/detail/throw_error.hpp"

#if defined(ASIO_HAS_IOCP)
# include "asio/detail/win_iocp_io_service.hpp"
#else
# include "asio/detail/scheduler.hpp"
#endif

#include "asio/detail/push_options.hpp"

namespace asio {

io_service::io_service()
  : impl_(create_impl())
{
}

io_service::io_service(std::size_t concurrency_hint)
  : impl_(create_impl(concurrency_hint))
{
}

io_service::impl_type& io_service::create_impl(std::size_t concurrency_hint)
{
  asio::detail::scoped_ptr<impl_type> impl(
      new impl_type(*this, concurrency_hint));
  asio::add_service<impl_type>(*this, impl.get());
  return *impl.release();
}

io_service::~io_service()
{
}

std::size_t io_service::run()
{
  asio::error_code ec;
  std::size_t s = impl_.run(ec);
  asio::detail::throw_error(ec);
  return s;
}

std::size_t io_service::run(asio::error_code& ec)
{
  return impl_.run(ec);
}

std::size_t io_service::run_one()
{
  asio::error_code ec;
  std::size_t s = impl_.run_one(ec);
  asio::detail::throw_error(ec);
  return s;
}

std::size_t io_service::run_one(asio::error_code& ec)
{
  return impl_.run_one(ec);
}

std::size_t io_service::poll()
{
  asio::error_code ec;
  std::size_t s = impl_.poll(ec);
  asio::detail::throw_error(ec);
  return s;
}

std::size_t io_service::poll(asio::error_code& ec)
{
  return impl_.poll(ec);
}

std::size_t io_service::poll_one()
{
  asio::error_code ec;
  std::size_t s = impl_.poll_one(ec);
  asio::detail::throw_error(ec);
  return s;
}

std::size_t io_service::poll_one(asio::error_code& ec)
{
  return impl_.poll_one(ec);
}

void io_service::stop()
{
  impl_.stop();
}

bool io_service::stopped() const
{
  return impl_.stopped();
}

void io_service::restart()
{
  impl_.restart();
}

io_service::service::service(asio::io_service& owner)
  : execution_context::service(owner)
{
}

io_service::service::~service()
{
}

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_IO_SERVICE_IPP
