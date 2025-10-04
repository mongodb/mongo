//
// ip/address_v6.hpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IP_ADDRESS_V6_HPP
#define BOOST_ASIO_IP_ADDRESS_V6_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <functional>
#include <string>
#include <boost/asio/detail/array.hpp>
#include <boost/asio/detail/cstdint.hpp>
#include <boost/asio/detail/socket_types.hpp>
#include <boost/asio/detail/string_view.hpp>
#include <boost/asio/detail/winsock_init.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/ip/address_v4.hpp>

#if !defined(BOOST_ASIO_NO_IOSTREAM)
# include <iosfwd>
#endif // !defined(BOOST_ASIO_NO_IOSTREAM)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace ip {

template <typename> class basic_address_iterator;

/// Type used for storing IPv6 scope IDs.
typedef uint_least32_t scope_id_type;

/// Implements IP version 6 style addresses.
/**
 * The boost::asio::ip::address_v6 class provides the ability to use and
 * manipulate IP version 6 addresses.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 */
class address_v6
{
public:
  /// The type used to represent an address as an array of bytes.
  /**
   * @note This type is defined in terms of the C++0x template @c std::array
   * when it is available. Otherwise, it uses @c boost:array.
   */
#if defined(GENERATING_DOCUMENTATION)
  typedef array<unsigned char, 16> bytes_type;
#else
  typedef boost::asio::detail::array<unsigned char, 16> bytes_type;
#endif

  /// Default constructor.
  /**
   * Initialises the @c address_v6 object such that:
   * @li <tt>to_bytes()</tt> yields <tt>{0, 0, ..., 0}</tt>; and
   * @li <tt>scope_id() == 0</tt>.
   */
  BOOST_ASIO_DECL address_v6() noexcept;

  /// Construct an address from raw bytes and scope ID.
  /**
   * Initialises the @c address_v6 object such that:
   * @li <tt>to_bytes() == bytes</tt>; and
   * @li <tt>this->scope_id() == scope_id</tt>.
   *
   * @throws out_of_range Thrown if any element in @c bytes is not in the range
   * <tt>0 - 0xFF</tt>. Note that no range checking is required for platforms
   * where <tt>std::numeric_limits<unsigned char>::max()</tt> is <tt>0xFF</tt>.
   */
  BOOST_ASIO_DECL explicit address_v6(const bytes_type& bytes,
      scope_id_type scope_id = 0);

  /// Copy constructor.
  BOOST_ASIO_DECL address_v6(const address_v6& other) noexcept;

  /// Move constructor.
  BOOST_ASIO_DECL address_v6(address_v6&& other) noexcept;

  /// Assign from another address.
  BOOST_ASIO_DECL address_v6& operator=(
      const address_v6& other) noexcept;

  /// Move-assign from another address.
  BOOST_ASIO_DECL address_v6& operator=(address_v6&& other) noexcept;

  /// The scope ID of the address.
  /**
   * Returns the scope ID associated with the IPv6 address.
   */
  scope_id_type scope_id() const noexcept
  {
    return scope_id_;
  }

  /// The scope ID of the address.
  /**
   * Modifies the scope ID associated with the IPv6 address.
   *
   * @param id The new scope ID.
   */
  void scope_id(scope_id_type id) noexcept
  {
    scope_id_ = id;
  }

  /// Get the address in bytes, in network byte order.
  BOOST_ASIO_DECL bytes_type to_bytes() const noexcept;

  /// Get the address as a string.
  BOOST_ASIO_DECL std::string to_string() const;

  /// Determine whether the address is a loopback address.
  /**
   * This function tests whether the address is the loopback address
   * <tt>::1</tt>.
   */
  BOOST_ASIO_DECL bool is_loopback() const noexcept;

  /// Determine whether the address is unspecified.
  /**
   * This function tests whether the address is the loopback address
   * <tt>::</tt>.
   */
  BOOST_ASIO_DECL bool is_unspecified() const noexcept;

  /// Determine whether the address is link local.
  BOOST_ASIO_DECL bool is_link_local() const noexcept;

  /// Determine whether the address is site local.
  BOOST_ASIO_DECL bool is_site_local() const noexcept;

  /// Determine whether the address is a mapped IPv4 address.
  BOOST_ASIO_DECL bool is_v4_mapped() const noexcept;

  /// Determine whether the address is a multicast address.
  BOOST_ASIO_DECL bool is_multicast() const noexcept;

  /// Determine whether the address is a global multicast address.
  BOOST_ASIO_DECL bool is_multicast_global() const noexcept;

  /// Determine whether the address is a link-local multicast address.
  BOOST_ASIO_DECL bool is_multicast_link_local() const noexcept;

  /// Determine whether the address is a node-local multicast address.
  BOOST_ASIO_DECL bool is_multicast_node_local() const noexcept;

  /// Determine whether the address is a org-local multicast address.
  BOOST_ASIO_DECL bool is_multicast_org_local() const noexcept;

  /// Determine whether the address is a site-local multicast address.
  BOOST_ASIO_DECL bool is_multicast_site_local() const noexcept;

  /// Compare two addresses for equality.
  BOOST_ASIO_DECL friend bool operator==(const address_v6& a1,
      const address_v6& a2) noexcept;

  /// Compare two addresses for inequality.
  friend bool operator!=(const address_v6& a1,
      const address_v6& a2) noexcept
  {
    return !(a1 == a2);
  }

  /// Compare addresses for ordering.
  BOOST_ASIO_DECL friend bool operator<(const address_v6& a1,
      const address_v6& a2) noexcept;

  /// Compare addresses for ordering.
  friend bool operator>(const address_v6& a1,
      const address_v6& a2) noexcept
  {
    return a2 < a1;
  }

  /// Compare addresses for ordering.
  friend bool operator<=(const address_v6& a1,
      const address_v6& a2) noexcept
  {
    return !(a2 < a1);
  }

  /// Compare addresses for ordering.
  friend bool operator>=(const address_v6& a1,
      const address_v6& a2) noexcept
  {
    return !(a1 < a2);
  }

  /// Obtain an address object that represents any address.
  /**
   * This functions returns an address that represents the "any" address
   * <tt>::</tt>.
   *
   * @returns A default-constructed @c address_v6 object.
   */
  static address_v6 any() noexcept
  {
    return address_v6();
  }

  /// Obtain an address object that represents the loopback address.
  /**
   * This function returns an address that represents the well-known loopback
   * address <tt>::1</tt>.
   */
  BOOST_ASIO_DECL static address_v6 loopback() noexcept;

private:
  friend class basic_address_iterator<address_v6>;

  // The underlying IPv6 address.
  boost::asio::detail::in6_addr_type addr_;

  // The scope ID associated with the address.
  scope_id_type scope_id_;
};

/// Create an IPv6 address from raw bytes and scope ID.
/**
 * @relates address_v6
 */
inline address_v6 make_address_v6(const address_v6::bytes_type& bytes,
    scope_id_type scope_id = 0)
{
  return address_v6(bytes, scope_id);
}

/// Create an IPv6 address from an IP address string.
/**
 * @relates address_v6
 */
BOOST_ASIO_DECL address_v6 make_address_v6(const char* str);

/// Create an IPv6 address from an IP address string.
/**
 * @relates address_v6
 */
BOOST_ASIO_DECL address_v6 make_address_v6(const char* str,
    boost::system::error_code& ec) noexcept;

/// Createan IPv6 address from an IP address string.
/**
 * @relates address_v6
 */
BOOST_ASIO_DECL address_v6 make_address_v6(const std::string& str);

/// Create an IPv6 address from an IP address string.
/**
 * @relates address_v6
 */
BOOST_ASIO_DECL address_v6 make_address_v6(const std::string& str,
    boost::system::error_code& ec) noexcept;

#if defined(BOOST_ASIO_HAS_STRING_VIEW) \
  || defined(GENERATING_DOCUMENTATION)

/// Create an IPv6 address from an IP address string.
/**
 * @relates address_v6
 */
BOOST_ASIO_DECL address_v6 make_address_v6(string_view str);

/// Create an IPv6 address from an IP address string.
/**
 * @relates address_v6
 */
BOOST_ASIO_DECL address_v6 make_address_v6(string_view str,
    boost::system::error_code& ec) noexcept;

#endif // defined(BOOST_ASIO_HAS_STRING_VIEW)
       //  || defined(GENERATING_DOCUMENTATION)

/// Tag type used for distinguishing overloads that deal in IPv4-mapped IPv6
/// addresses.
enum v4_mapped_t { v4_mapped };

/// Create an IPv4 address from a IPv4-mapped IPv6 address.
/**
 * @relates address_v4
 */
BOOST_ASIO_DECL address_v4 make_address_v4(
    v4_mapped_t, const address_v6& v6_addr);

/// Create an IPv4-mapped IPv6 address from an IPv4 address.
/**
 * @relates address_v6
 */
BOOST_ASIO_DECL address_v6 make_address_v6(
    v4_mapped_t, const address_v4& v4_addr);

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
 * @relates boost::asio::ip::address_v6
 */
template <typename Elem, typename Traits>
std::basic_ostream<Elem, Traits>& operator<<(
    std::basic_ostream<Elem, Traits>& os, const address_v6& addr);

#endif // !defined(BOOST_ASIO_NO_IOSTREAM)

} // namespace ip
} // namespace asio
} // namespace boost

namespace std {

template <>
struct hash<boost::asio::ip::address_v6>
{
  std::size_t operator()(const boost::asio::ip::address_v6& addr)
    const noexcept
  {
    const boost::asio::ip::address_v6::bytes_type bytes = addr.to_bytes();
    std::size_t result = static_cast<std::size_t>(addr.scope_id());
    combine_4_bytes(result, &bytes[0]);
    combine_4_bytes(result, &bytes[4]);
    combine_4_bytes(result, &bytes[8]);
    combine_4_bytes(result, &bytes[12]);
    return result;
  }

private:
  static void combine_4_bytes(std::size_t& seed, const unsigned char* bytes)
  {
    const std::size_t bytes_hash =
      (static_cast<std::size_t>(bytes[0]) << 24) |
      (static_cast<std::size_t>(bytes[1]) << 16) |
      (static_cast<std::size_t>(bytes[2]) << 8) |
      (static_cast<std::size_t>(bytes[3]));
    seed ^= bytes_hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }
};

} // namespace std

#include <boost/asio/detail/pop_options.hpp>

#include <boost/asio/ip/impl/address_v6.hpp>
#if defined(BOOST_ASIO_HEADER_ONLY)
# include <boost/asio/ip/impl/address_v6.ipp>
#endif // defined(BOOST_ASIO_HEADER_ONLY)

#endif // BOOST_ASIO_IP_ADDRESS_V6_HPP
