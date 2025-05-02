//
// ssl/impl/error.ipp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_SSL_IMPL_ERROR_IPP
#define BOOST_ASIO_SSL_IMPL_ERROR_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/detail/openssl_init.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace error {
namespace detail {

class ssl_category : public boost::system::error_category
{
public:
  const char* name() const noexcept
  {
    return "asio.ssl";
  }

  std::string message(int value) const
  {
    const char* reason = ::ERR_reason_error_string(value);
    if (reason)
    {
      const char* lib = ::ERR_lib_error_string(value);
#if (OPENSSL_VERSION_NUMBER < 0x30000000L)
      const char* func = ::ERR_func_error_string(value);
#else // (OPENSSL_VERSION_NUMBER < 0x30000000L)
      const char* func = 0;
#endif // (OPENSSL_VERSION_NUMBER < 0x30000000L)
      std::string result(reason);
      if (lib || func)
      {
        result += " (";
        if (lib)
          result += lib;
        if (lib && func)
          result += ", ";
        if (func)
          result += func;
        result += ")";
      }
      return result;
    }
    return "asio.ssl error";
  }
};

} // namespace detail

const boost::system::error_category& get_ssl_category()
{
  static detail::ssl_category instance;
  return instance;
}

} // namespace error
namespace ssl {
namespace error {

#if (OPENSSL_VERSION_NUMBER < 0x10100000L) && !defined(OPENSSL_IS_BORINGSSL)

const boost::system::error_category& get_stream_category()
{
  return boost::asio::error::get_ssl_category();
}

#else

namespace detail {

class stream_category : public boost::system::error_category
{
public:
  const char* name() const noexcept
  {
    return "asio.ssl.stream";
  }

  std::string message(int value) const
  {
    switch (value)
    {
    case stream_truncated: return "stream truncated";
    case unspecified_system_error: return "unspecified system error";
    case unexpected_result: return "unexpected result";
    default: return "asio.ssl.stream error";
    }
  }
};

} // namespace detail

const boost::system::error_category& get_stream_category()
{
  static detail::stream_category instance;
  return instance;
}

#endif

} // namespace error
} // namespace ssl
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_SSL_IMPL_ERROR_IPP
