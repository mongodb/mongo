//
// ip/resolver_query_base.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IP_RESOLVER_QUERY_BASE_HPP
#define BOOST_ASIO_IP_RESOLVER_QUERY_BASE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/asio/detail/socket_types.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace ip {

/// The resolver_query_base class is used as a base for the
/// basic_resolver_query class templates to provide a common place to define
/// the flag constants.
class resolver_query_base
{
public:
#if defined(GENERATING_DOCUMENTATION)
  /// A bitmask type (C++ Std [lib.bitmask.types]).
  typedef unspecified flags;

  /// Determine the canonical name of the host specified in the query.
  static const flags canonical_name = implementation_defined;

  /// Indicate that returned endpoint is intended for use as a locally bound
  /// socket endpoint.
  static const flags passive = implementation_defined;

  /// Host name should be treated as a numeric string defining an IPv4 or IPv6
  /// address and no name resolution should be attempted.
  static const flags numeric_host = implementation_defined;

  /// Service name should be treated as a numeric string defining a port number
  /// and no name resolution should be attempted.
  static const flags numeric_service = implementation_defined;

  /// If the query protocol family is specified as IPv6, return IPv4-mapped
  /// IPv6 addresses on finding no IPv6 addresses.
  static const flags v4_mapped = implementation_defined;

  /// If used with v4_mapped, return all matching IPv6 and IPv4 addresses.
  static const flags all_matching = implementation_defined;

  /// Only return IPv4 addresses if a non-loopback IPv4 address is configured
  /// for the system. Only return IPv6 addresses if a non-loopback IPv6 address
  /// is configured for the system.
  static const flags address_configured = implementation_defined;
#else
  enum flags
  {
    canonical_name = AI_CANONNAME,
    passive = AI_PASSIVE,
    numeric_host = AI_NUMERICHOST,
# if defined(AI_NUMERICSERV)
    numeric_service = AI_NUMERICSERV,
# else
    numeric_service = 0,
# endif
    // Note: QNX Neutrino 6.3 defines AI_V4MAPPED, AI_ALL and AI_ADDRCONFIG but
    // does not implement them. Therefore they are specifically excluded here.
# if defined(AI_V4MAPPED) && !defined(__QNXNTO__)
    v4_mapped = AI_V4MAPPED,
# else
    v4_mapped = 0,
# endif
# if defined(AI_ALL) && !defined(__QNXNTO__)
    all_matching = AI_ALL,
# else
    all_matching = 0,
# endif
# if defined(AI_ADDRCONFIG) && !defined(__QNXNTO__)
    address_configured = AI_ADDRCONFIG
# else
    address_configured = 0
# endif
  };

  // Implement bitmask operations as shown in C++ Std [lib.bitmask.types].

  friend flags operator&(flags x, flags y)
  {
    return static_cast<flags>(
        static_cast<unsigned int>(x) & static_cast<unsigned int>(y));
  }

  friend flags operator|(flags x, flags y)
  {
    return static_cast<flags>(
        static_cast<unsigned int>(x) | static_cast<unsigned int>(y));
  }

  friend flags operator^(flags x, flags y)
  {
    return static_cast<flags>(
        static_cast<unsigned int>(x) ^ static_cast<unsigned int>(y));
  }

  friend flags operator~(flags x)
  {
    return static_cast<flags>(static_cast<unsigned int>(~x));
  }

  friend flags& operator&=(flags& x, flags y)
  {
    x = x & y;
    return x;
  }

  friend flags& operator|=(flags& x, flags y)
  {
    x = x | y;
    return x;
  }

  friend flags& operator^=(flags& x, flags y)
  {
    x = x ^ y;
    return x;
  }
#endif

protected:
  /// Protected destructor to prevent deletion through this type.
  ~resolver_query_base()
  {
  }

#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
private:
  // Workaround to enable the empty base optimisation with Borland C++.
  char dummy_;
#endif
};

} // namespace ip
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IP_RESOLVER_QUERY_BASE_HPP
