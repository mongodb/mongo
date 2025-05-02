//
// ip/address.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IP_ADDRESS_HPP
#define BOOST_ASIO_IP_ADDRESS_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <functional>
#include <string>
#include <boost/asio/detail/throw_exception.hpp>
#include <boost/asio/detail/string_view.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/bad_address_cast.hpp>

#if !defined(BOOST_ASIO_NO_IOSTREAM)
# include <iosfwd>
#endif // !defined(BOOST_ASIO_NO_IOSTREAM)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace ip {

/// Implements version-independent IP addresses.
/**
 * The boost::asio::ip::address class provides the ability to use either IP
 * version 4 or version 6 addresses.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 */
class address
{
public:
  /// Default constructor.
  BOOST_ASIO_DECL address() noexcept;

  /// Construct an address from an IPv4 address.
  BOOST_ASIO_DECL address(
      const boost::asio::ip::address_v4& ipv4_address) noexcept;

  /// Construct an address from an IPv6 address.
  BOOST_ASIO_DECL address(
      const boost::asio::ip::address_v6& ipv6_address) noexcept;

  /// Copy constructor.
  BOOST_ASIO_DECL address(const address& other) noexcept;

  /// Move constructor.
  BOOST_ASIO_DECL address(address&& other) noexcept;

  /// Assign from another address.
  BOOST_ASIO_DECL address& operator=(const address& other) noexcept;

  /// Move-assign from another address.
  BOOST_ASIO_DECL address& operator=(address&& other) noexcept;

  /// Assign from an IPv4 address.
  BOOST_ASIO_DECL address& operator=(
      const boost::asio::ip::address_v4& ipv4_address) noexcept;

  /// Assign from an IPv6 address.
  BOOST_ASIO_DECL address& operator=(
      const boost::asio::ip::address_v6& ipv6_address) noexcept;

  /// Get whether the address is an IP version 4 address.
  bool is_v4() const noexcept
  {
    return type_ == ipv4;
  }

  /// Get whether the address is an IP version 6 address.
  bool is_v6() const noexcept
  {
    return type_ == ipv6;
  }

  /// Get the address as an IP version 4 address.
  BOOST_ASIO_DECL boost::asio::ip::address_v4 to_v4() const;

  /// Get the address as an IP version 6 address.
  BOOST_ASIO_DECL boost::asio::ip::address_v6 to_v6() const;

  /// Get the address as a string.
  BOOST_ASIO_DECL std::string to_string() const;

  /// Determine whether the address is a loopback address.
  BOOST_ASIO_DECL bool is_loopback() const noexcept;

  /// Determine whether the address is unspecified.
  BOOST_ASIO_DECL bool is_unspecified() const noexcept;

  /// Determine whether the address is a multicast address.
  BOOST_ASIO_DECL bool is_multicast() const noexcept;

  /// Compare two addresses for equality.
  BOOST_ASIO_DECL friend bool operator==(const address& a1,
      const address& a2) noexcept;

  /// Compare two addresses for inequality.
  friend bool operator!=(const address& a1,
      const address& a2) noexcept
  {
    return !(a1 == a2);
  }

  /// Compare addresses for ordering.
  BOOST_ASIO_DECL friend bool operator<(const address& a1,
      const address& a2) noexcept;

  /// Compare addresses for ordering.
  friend bool operator>(const address& a1,
      const address& a2) noexcept
  {
    return a2 < a1;
  }

  /// Compare addresses for ordering.
  friend bool operator<=(const address& a1,
      const address& a2) noexcept
  {
    return !(a2 < a1);
  }

  /// Compare addresses for ordering.
  friend bool operator>=(const address& a1,
      const address& a2) noexcept
  {
    return !(a1 < a2);
  }

private:
  // The type of the address.
  enum { ipv4, ipv6 } type_;

  // The underlying IPv4 address.
  boost::asio::ip::address_v4 ipv4_address_;

  // The underlying IPv6 address.
  boost::asio::ip::address_v6 ipv6_address_;
};

/// Create an address from an IPv4 address string in dotted decimal form,
/// or from an IPv6 address in hexadecimal notation.
/**
 * @relates address
 */
BOOST_ASIO_DECL address make_address(const char* str);

/// Create an address from an IPv4 address string in dotted decimal form,
/// or from an IPv6 address in hexadecimal notation.
/**
 * @relates address
 */
BOOST_ASIO_DECL address make_address(const char* str,
    boost::system::error_code& ec) noexcept;

/// Create an address from an IPv4 address string in dotted decimal form,
/// or from an IPv6 address in hexadecimal notation.
/**
 * @relates address
 */
BOOST_ASIO_DECL address make_address(const std::string& str);

/// Create an address from an IPv4 address string in dotted decimal form,
/// or from an IPv6 address in hexadecimal notation.
/**
 * @relates address
 */
BOOST_ASIO_DECL address make_address(const std::string& str,
    boost::system::error_code& ec) noexcept;

#if defined(BOOST_ASIO_HAS_STRING_VIEW) \
  || defined(GENERATING_DOCUMENTATION)

/// Create an address from an IPv4 address string in dotted decimal form,
/// or from an IPv6 address in hexadecimal notation.
/**
 * @relates address
 */
BOOST_ASIO_DECL address make_address(string_view str);

/// Create an address from an IPv4 address string in dotted decimal form,
/// or from an IPv6 address in hexadecimal notation.
/**
 * @relates address
 */
BOOST_ASIO_DECL address make_address(string_view str,
    boost::system::error_code& ec) noexcept;

#endif // defined(BOOST_ASIO_HAS_STRING_VIEW)
       //  || defined(GENERATING_DOCUMENTATION)

#if !defined(BOOST_ASIO_NO_IOSTREAM)

/// Output an address as a string.
/**
 * Used to output a human-readable string for a specified address.
 *
 * @param os The output stream to which the string will be written.
 *
 * @param addr The address to be written.
 *
 * @return The output stream.
 *
 * @relates boost::asio::ip::address
 */
template <typename Elem, typename Traits>
std::basic_ostream<Elem, Traits>& operator<<(
    std::basic_ostream<Elem, Traits>& os, const address& addr);

#endif // !defined(BOOST_ASIO_NO_IOSTREAM)

} // namespace ip
} // namespace asio
} // namespace boost

namespace std {

template <>
struct hash<boost::asio::ip::address>
{
  std::size_t operator()(const boost::asio::ip::address& addr)
    const noexcept
  {
    return addr.is_v4()
      ? std::hash<boost::asio::ip::address_v4>()(addr.to_v4())
      : std::hash<boost::asio::ip::address_v6>()(addr.to_v6());
  }
};

} // namespace std

#include <boost/asio/detail/pop_options.hpp>

#include <boost/asio/ip/impl/address.hpp>
#if defined(BOOST_ASIO_HEADER_ONLY)
# include <boost/asio/ip/impl/address.ipp>
#endif // defined(BOOST_ASIO_HEADER_ONLY)

#endif // BOOST_ASIO_IP_ADDRESS_HPP
