//
// ip/impl/address_v4.ipp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IP_IMPL_ADDRESS_V4_IPP
#define BOOST_ASIO_IP_IMPL_ADDRESS_V4_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <climits>
#include <limits>
#include <stdexcept>
#include <boost/asio/error.hpp>
#include <boost/asio/detail/socket_ops.hpp>
#include <boost/asio/detail/throw_error.hpp>
#include <boost/asio/detail/throw_exception.hpp>
#include <boost/asio/ip/address_v4.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace ip {

address_v4::address_v4(const address_v4::bytes_type& bytes)
{
#if UCHAR_MAX > 0xFF
  if (bytes[0] > 0xFF || bytes[1] > 0xFF
      || bytes[2] > 0xFF || bytes[3] > 0xFF)
  {
    std::out_of_range ex("address_v4 from bytes_type");
    boost::asio::detail::throw_exception(ex);
  }
#endif // UCHAR_MAX > 0xFF

  using namespace std; // For memcpy.
  memcpy(&addr_.s_addr, bytes.data(), 4);
}

address_v4::address_v4(address_v4::uint_type addr)
{
  if ((std::numeric_limits<uint_type>::max)() > 0xFFFFFFFF)
  {
    std::out_of_range ex("address_v4 from unsigned integer");
    boost::asio::detail::throw_exception(ex);
  }

  addr_.s_addr = boost::asio::detail::socket_ops::host_to_network_long(
      static_cast<boost::asio::detail::u_long_type>(addr));
}

address_v4::bytes_type address_v4::to_bytes() const noexcept
{
  using namespace std; // For memcpy.
  bytes_type bytes;
  memcpy(bytes.data(), &addr_.s_addr, 4);
  return bytes;
}

address_v4::uint_type address_v4::to_uint() const noexcept
{
  return boost::asio::detail::socket_ops::network_to_host_long(addr_.s_addr);
}

std::string address_v4::to_string() const
{
  boost::system::error_code ec;
  char addr_str[boost::asio::detail::max_addr_v4_str_len];
  const char* addr =
    boost::asio::detail::socket_ops::inet_ntop(
        BOOST_ASIO_OS_DEF(AF_INET), &addr_, addr_str,
        boost::asio::detail::max_addr_v4_str_len, 0, ec);
  if (addr == 0)
    boost::asio::detail::throw_error(ec);
  return addr;
}

bool address_v4::is_loopback() const noexcept
{
  return (to_uint() & 0xFF000000) == 0x7F000000;
}

bool address_v4::is_unspecified() const noexcept
{
  return to_uint() == 0;
}

bool address_v4::is_multicast() const noexcept
{
  return (to_uint() & 0xF0000000) == 0xE0000000;
}

address_v4 make_address_v4(const char* str)
{
  boost::system::error_code ec;
  address_v4 addr = make_address_v4(str, ec);
  boost::asio::detail::throw_error(ec);
  return addr;
}

address_v4 make_address_v4(const char* str,
    boost::system::error_code& ec) noexcept
{
  address_v4::bytes_type bytes;
  if (boost::asio::detail::socket_ops::inet_pton(
        BOOST_ASIO_OS_DEF(AF_INET), str, &bytes, 0, ec) <= 0)
    return address_v4();
  return address_v4(bytes);
}

address_v4 make_address_v4(const std::string& str)
{
  return make_address_v4(str.c_str());
}

address_v4 make_address_v4(const std::string& str,
    boost::system::error_code& ec) noexcept
{
  return make_address_v4(str.c_str(), ec);
}

#if defined(BOOST_ASIO_HAS_STRING_VIEW)

address_v4 make_address_v4(string_view str)
{
  return make_address_v4(static_cast<std::string>(str));
}

address_v4 make_address_v4(string_view str,
    boost::system::error_code& ec) noexcept
{
  return make_address_v4(static_cast<std::string>(str), ec);
}

#endif // defined(BOOST_ASIO_HAS_STRING_VIEW)

} // namespace ip
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IP_IMPL_ADDRESS_V4_IPP
