//
// ip/address_range_v6.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//                         Oliver Kowalke (oliver dot kowalke at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IP_ADDRESS_RANGE_v6_HPP
#define ASIO_IP_ADDRESS_RANGE_v6_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/ip/address_iterator_v6.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ip {

/// Represents a range of IPv6 addresses.
/**
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 */
class address_range_v6
{
public:
  /// The type of an iterator that points into the range.
  typedef address_iterator_v6 iterator;

  /// Construct an empty range.
  address_range_v6() ASIO_NOEXCEPT
    : begin_(address_v6()),
      end_(address_v6())
  {
  }

  /// Construct an range that represents the given range of addresses.
  explicit address_range_v6(const address_iterator_v6& first,
      const address_iterator_v6& last) ASIO_NOEXCEPT
    : begin_(first),
      end_(last)
  {
  }

  /// Copy constructor.
  address_range_v6(const address_range_v6& other) ASIO_NOEXCEPT
    : begin_(other.begin_),
      end_(other.end_)
  {
  }

#if defined(ASIO_HAS_MOVE)
  /// Move constructor.
  address_range_v6(address_range_v6&& other) ASIO_NOEXCEPT
    : begin_(ASIO_MOVE_CAST(address_iterator_v6)(other.begin_)),
      end_(ASIO_MOVE_CAST(address_iterator_v6)(other.end_))
  {
  }
#endif // defined(ASIO_HAS_MOVE)

  /// Assignment operator.
  address_range_v6& operator=(
      const address_range_v6& other) ASIO_NOEXCEPT
  {
    begin_ = other.begin_;
    end_ = other.end_;
    return *this;
  }

#if defined(ASIO_HAS_MOVE)
  /// Move assignment operator.
  address_range_v6& operator=(
      address_range_v6&& other) ASIO_NOEXCEPT
  {
    begin_ = ASIO_MOVE_CAST(address_iterator_v6)(other.begin_);
    end_ = ASIO_MOVE_CAST(address_iterator_v6)(other.end_);
    return *this;
  }
#endif // defined(ASIO_HAS_MOVE)

  /// Obtain an iterator that points to the start of the range.
  iterator begin() const ASIO_NOEXCEPT
  {
    return begin_;
  }

  /// Obtain an iterator that points to the end of the range.
  iterator end() const ASIO_NOEXCEPT
  {
    return end_;
  }

  /// Determine whether the range is empty.
  bool empty() const ASIO_NOEXCEPT
  {
    return begin_ == end_;
  }

  /// Find an address in the range.
  iterator find(const address_v6& addr) const ASIO_NOEXCEPT
  {
    return addr >= *begin_ && addr < *end_ ? iterator(addr) : end_;
  }

private:
  address_iterator_v6 begin_;
  address_iterator_v6 end_;
};

} // namespace ip
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IP_ADDRESS_RANGE_V6_HPP
