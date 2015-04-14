//
// ip/address_iterator_v6.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//                         Oliver Kowalke (oliver dot kowalke at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IP_ADDRESS_ITERATOR_V6_HPP
#define ASIO_IP_ADDRESS_ITERATOR_V6_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/ip/address_v6.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ip {

/// An input iterator that can be used for traversing IPv6 addresses.
/**
 * In addition to satisfying the input iterator requirements, this iterator
 * also supports decrement.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 */
class address_iterator_v6
{
public:
  /// The type of the elements pointed to by the iterator.
  typedef address_v6 value_type;

  /// Distance between two iterators.
  typedef std::ptrdiff_t difference_type;

  /// The type of a pointer to an element pointed to by the iterator.
  typedef const address_v6* pointer;

  /// The type of a reference to an element pointed to by the iterator.
  typedef const address_v6& reference;

  /// Denotes that the iterator satisfies the input iterator requirements.
  typedef std::input_iterator_tag iterator_category;

  /// Construct an iterator that points to the specified address.
  address_iterator_v6(const address_v6& addr) ASIO_NOEXCEPT
    : address_(addr)
  {
  }

  /// Copy constructor.
  address_iterator_v6(const address_iterator_v6& other) ASIO_NOEXCEPT
    : address_(other.address_)
  {
  }

#if defined(ASIO_HAS_MOVE)
  /// Move constructor.
  address_iterator_v6(address_iterator_v6&& other) ASIO_NOEXCEPT
    : address_(ASIO_MOVE_CAST(address_v6)(other.address_))
  {
  }
#endif // defined(ASIO_HAS_MOVE)

  /// Assignment operator.
  address_iterator_v6& operator=(
      const address_iterator_v6& other) ASIO_NOEXCEPT
  {
    address_ = other.address_;
    return *this;
  }

#if defined(ASIO_HAS_MOVE)
  /// Move assignment operator.
  address_iterator_v6& operator=(
      address_iterator_v6&& other) ASIO_NOEXCEPT
  {
    address_ = ASIO_MOVE_CAST(address_v6)(other.address_);
    return *this;
  }
#endif // defined(ASIO_HAS_MOVE)

  /// Dereference the iterator.
  const address_v6& operator*() const ASIO_NOEXCEPT
  {
    return address_;
  }

  /// Dereference the iterator.
  const address_v6* operator->() const ASIO_NOEXCEPT
  {
    return &address_;
  }

  /// Pre-increment operator.
  address_iterator_v6& operator++() ASIO_NOEXCEPT
  {
    for (int i = 15; i >= 0; --i)
    {
      if (address_.addr_.s6_addr[i] < 0xFF)
      {
        ++address_.addr_.s6_addr[i];
        break;
      }

      address_.addr_.s6_addr[i] = 0;
    }

    return *this;
  }

  /// Post-increment operator.
  address_iterator_v6 operator++(int) ASIO_NOEXCEPT
  {
    address_iterator_v6 tmp(*this);
    ++*this;
    return tmp;
  }

  /// Pre-decrement operator.
  address_iterator_v6& operator--() ASIO_NOEXCEPT
  {
    for (int i = 15; i >= 0; --i)
    {
      if (address_.addr_.s6_addr[i] > 0)
      {
        --address_.addr_.s6_addr[i];
        break;
      }

      address_.addr_.s6_addr[i] = 0xFF;
    }

    return *this;
  }

  /// Post-decrement operator.
  address_iterator_v6 operator--(int)
  {
    address_iterator_v6 tmp(*this);
    --*this;
    return tmp;
  }

  /// Compare two addresses for equality.
  friend bool operator==(const address_iterator_v6& a,
      const address_iterator_v6& b)
  {
    return a.address_ == b.address_;
  }

  /// Compare two addresses for inequality.
  friend bool operator!=(const address_iterator_v6& a,
      const address_iterator_v6& b)
  {
    return a.address_ != b.address_;
  }

private:
  address_v6 address_;
};

} // namespace ip
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IP_ADDRESS_ITERATOR_V6_HPP
