//
// recycling_allocator.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_RECYCLING_ALLOCATOR_HPP
#define BOOST_ASIO_RECYCLING_ALLOCATOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/recycling_allocator.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

/// An allocator that caches memory blocks in thread-local storage for reuse.
/**
 * The @recycling_allocator uses a simple strategy where a limited number of
 * small memory blocks are cached in thread-local storage, if the current
 * thread is running an @c io_context or is part of a @c thread_pool.
 */
template <typename T>
class recycling_allocator
{
public:
  /// The type of object allocated by the recycling allocator.
  typedef T value_type;

  /// Rebind the allocator to another value_type.
  template <typename U>
  struct rebind
  {
    /// The rebound @c allocator type.
    typedef recycling_allocator<U> other;
  };

  /// Default constructor.
  BOOST_ASIO_CONSTEXPR recycling_allocator() BOOST_ASIO_NOEXCEPT
  {
  }

  /// Converting constructor.
  template <typename U>
  BOOST_ASIO_CONSTEXPR recycling_allocator(
      const recycling_allocator<U>&) BOOST_ASIO_NOEXCEPT
  {
  }

  /// Equality operator. Always returns true.
  BOOST_ASIO_CONSTEXPR bool operator==(
      const recycling_allocator&) const BOOST_ASIO_NOEXCEPT
  {
    return true;
  }

  /// Inequality operator. Always returns false.
  BOOST_ASIO_CONSTEXPR bool operator!=(
      const recycling_allocator&) const BOOST_ASIO_NOEXCEPT
  {
    return false;
  }

  /// Allocate memory for the specified number of values.
  T* allocate(std::size_t n)
  {
    return detail::recycling_allocator<T>().allocate(n);
  }

  /// Deallocate memory for the specified number of values.
  void deallocate(T* p, std::size_t n)
  {
    detail::recycling_allocator<T>().deallocate(p, n);
  }
};

/// A proto-allocator that caches memory blocks in thread-local storage for
/// reuse.
/**
 * The @recycling_allocator uses a simple strategy where a limited number of
 * small memory blocks are cached in thread-local storage, if the current
 * thread is running an @c io_context or is part of a @c thread_pool.
 */
template <>
class recycling_allocator<void>
{
public:
  /// No values are allocated by a proto-allocator.
  typedef void value_type;

  /// Rebind the allocator to another value_type.
  template <typename U>
  struct rebind
  {
    /// The rebound @c allocator type.
    typedef recycling_allocator<U> other;
  };

  /// Default constructor.
  BOOST_ASIO_CONSTEXPR recycling_allocator() BOOST_ASIO_NOEXCEPT
  {
  }

  /// Converting constructor.
  template <typename U>
  BOOST_ASIO_CONSTEXPR recycling_allocator(
      const recycling_allocator<U>&) BOOST_ASIO_NOEXCEPT
  {
  }

  /// Equality operator. Always returns true.
  BOOST_ASIO_CONSTEXPR bool operator==(
      const recycling_allocator&) const BOOST_ASIO_NOEXCEPT
  {
    return true;
  }

  /// Inequality operator. Always returns false.
  BOOST_ASIO_CONSTEXPR bool operator!=(
      const recycling_allocator&) const BOOST_ASIO_NOEXCEPT
  {
    return false;
  }
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_RECYCLING_ALLOCATOR_HPP
