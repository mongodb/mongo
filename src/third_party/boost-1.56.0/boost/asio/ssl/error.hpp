//
// ssl/error.hpp
// ~~~~~~~~~~~~~
//
// Copyright (c) 2003-2014 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_SSL_ERROR_HPP
#define BOOST_ASIO_SSL_ERROR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/system/error_code.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace error {

enum ssl_errors
{
};

extern BOOST_ASIO_DECL
const boost::system::error_category& get_ssl_category();

static const boost::system::error_category& ssl_category
  = boost::asio::error::get_ssl_category();

} // namespace error
} // namespace asio
} // namespace boost

namespace boost {
namespace system {

template<> struct is_error_code_enum<boost::asio::error::ssl_errors>
{
  static const bool value = true;
};

} // namespace system
} // namespace boost

namespace boost {
namespace asio {
namespace error {

inline boost::system::error_code make_error_code(ssl_errors e)
{
  return boost::system::error_code(
      static_cast<int>(e), get_ssl_category());
}

} // namespace error
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#if defined(BOOST_ASIO_HEADER_ONLY)
# include <boost/asio/ssl/impl/error.ipp>
#endif // defined(BOOST_ASIO_HEADER_ONLY)

#endif // BOOST_ASIO_SSL_ERROR_HPP
