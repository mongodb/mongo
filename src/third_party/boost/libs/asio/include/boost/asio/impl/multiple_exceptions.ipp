//
// impl/multiple_exceptions.ipp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_MULTIPLE_EXCEPTIONS_IPP
#define BOOST_ASIO_IMPL_MULTIPLE_EXCEPTIONS_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/multiple_exceptions.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

multiple_exceptions::multiple_exceptions(
    std::exception_ptr first) noexcept
  : first_(static_cast<std::exception_ptr&&>(first))
{
}

const char* multiple_exceptions::what() const noexcept
{
  return "multiple exceptions";
}

std::exception_ptr multiple_exceptions::first_exception() const
{
  return first_;
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_MULTIPLE_EXCEPTIONS_IPP
