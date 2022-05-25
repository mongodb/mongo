//
// impl/any_io_executor.ipp
// ~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_ANY_IO_EXECUTOR_IPP
#define BOOST_ASIO_IMPL_ANY_IO_EXECUTOR_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if !defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)

#include <boost/asio/any_io_executor.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

any_io_executor::any_io_executor() BOOST_ASIO_NOEXCEPT
  : base_type()
{
}

any_io_executor::any_io_executor(nullptr_t) BOOST_ASIO_NOEXCEPT
  : base_type(nullptr_t())
{
}

any_io_executor::any_io_executor(const any_io_executor& e) BOOST_ASIO_NOEXCEPT
  : base_type(static_cast<const base_type&>(e))
{
}

#if defined(BOOST_ASIO_HAS_MOVE)
any_io_executor::any_io_executor(any_io_executor&& e) BOOST_ASIO_NOEXCEPT
  : base_type(static_cast<base_type&&>(e))
{
}
#endif // defined(BOOST_ASIO_HAS_MOVE)

any_io_executor& any_io_executor::operator=(
    const any_io_executor& e) BOOST_ASIO_NOEXCEPT
{
  base_type::operator=(static_cast<const base_type&>(e));
  return *this;
}

#if defined(BOOST_ASIO_HAS_MOVE)
any_io_executor& any_io_executor::operator=(
    any_io_executor&& e) BOOST_ASIO_NOEXCEPT
{
  base_type::operator=(static_cast<base_type&&>(e));
  return *this;
}
#endif // defined(BOOST_ASIO_HAS_MOVE)

any_io_executor& any_io_executor::operator=(nullptr_t)
{
  base_type::operator=(nullptr_t());
  return *this;
}

any_io_executor::~any_io_executor()
{
}

void any_io_executor::swap(any_io_executor& other) BOOST_ASIO_NOEXCEPT
{
  static_cast<base_type&>(*this).swap(static_cast<base_type&>(other));
}

template <>
any_io_executor any_io_executor::require(
    const execution::blocking_t::never_t& p, int) const
{
  return static_cast<const base_type&>(*this).require(p);
}

template <>
any_io_executor any_io_executor::prefer(
    const execution::blocking_t::possibly_t& p, int) const
{
  return static_cast<const base_type&>(*this).prefer(p);
}

template <>
any_io_executor any_io_executor::prefer(
    const execution::outstanding_work_t::tracked_t& p, int) const
{
  return static_cast<const base_type&>(*this).prefer(p);
}

template <>
any_io_executor any_io_executor::prefer(
    const execution::outstanding_work_t::untracked_t& p, int) const
{
  return static_cast<const base_type&>(*this).prefer(p);
}

template <>
any_io_executor any_io_executor::prefer(
    const execution::relationship_t::fork_t& p, int) const
{
  return static_cast<const base_type&>(*this).prefer(p);
}

template <>
any_io_executor any_io_executor::prefer(
    const execution::relationship_t::continuation_t& p, int) const
{
  return static_cast<const base_type&>(*this).prefer(p);
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // !defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)

#endif // BOOST_ASIO_IMPL_ANY_IO_EXECUTOR_IPP
