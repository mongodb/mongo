//
// impl/io_context.ipp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_IO_CONTEXT_IPP
#define BOOST_ASIO_IMPL_IO_CONTEXT_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/config.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>
#include <boost/asio/detail/limits.hpp>
#include <boost/asio/detail/scoped_ptr.hpp>
#include <boost/asio/detail/service_registry.hpp>
#include <boost/asio/detail/throw_error.hpp>

#if defined(BOOST_ASIO_HAS_IOCP)
# include <boost/asio/detail/win_iocp_io_context.hpp>
#else
# include <boost/asio/detail/scheduler.hpp>
#endif

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

io_context::io_context()
  : execution_context(config_from_concurrency_hint()),
    impl_(add_impl(new impl_type(*this, false)))
{
}

io_context::io_context(int concurrency_hint)
  : execution_context(config_from_concurrency_hint(concurrency_hint)),
    impl_(add_impl(new impl_type(*this, false)))
{
}

io_context::io_context(const execution_context::service_maker& initial_services)
  : execution_context(initial_services),
    impl_(add_impl(new impl_type(*this, false)))
{
}

io_context::impl_type& io_context::add_impl(io_context::impl_type* impl)
{
  boost::asio::detail::scoped_ptr<impl_type> scoped_impl(impl);
  boost::asio::add_service<impl_type>(*this, scoped_impl.get());
  return *scoped_impl.release();
}

io_context::~io_context()
{
  shutdown();
}

io_context::count_type io_context::run()
{
  boost::system::error_code ec;
  count_type s = impl_.run(ec);
  boost::asio::detail::throw_error(ec);
  return s;
}

io_context::count_type io_context::run_one()
{
  boost::system::error_code ec;
  count_type s = impl_.run_one(ec);
  boost::asio::detail::throw_error(ec);
  return s;
}

io_context::count_type io_context::poll()
{
  boost::system::error_code ec;
  count_type s = impl_.poll(ec);
  boost::asio::detail::throw_error(ec);
  return s;
}

io_context::count_type io_context::poll_one()
{
  boost::system::error_code ec;
  count_type s = impl_.poll_one(ec);
  boost::asio::detail::throw_error(ec);
  return s;
}

void io_context::stop()
{
  impl_.stop();
}

bool io_context::stopped() const
{
  return impl_.stopped();
}

void io_context::restart()
{
  impl_.restart();
}

io_context::service::service(boost::asio::io_context& owner)
  : execution_context::service(owner)
{
}

io_context::service::~service()
{
}

void io_context::service::shutdown()
{
}

void io_context::service::notify_fork(io_context::fork_event)
{
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_IO_CONTEXT_IPP
