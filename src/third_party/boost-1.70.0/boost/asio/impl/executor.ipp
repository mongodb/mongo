//
// impl/executor.ipp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_EXECUTOR_IPP
#define BOOST_ASIO_IMPL_EXECUTOR_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/executor.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

bad_executor::bad_executor() BOOST_ASIO_NOEXCEPT
{
}

const char* bad_executor::what() const BOOST_ASIO_NOEXCEPT_OR_NOTHROW
{
  return "bad executor";
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_EXECUTOR_IPP
