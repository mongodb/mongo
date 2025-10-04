//
// detail/socket_option.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_SOCKET_OPTION_HPP
#define ASIO_DETAIL_SOCKET_OPTION_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/chrono.hpp"
#include "asio/detail/config.hpp"
#include <cstddef>
#include <stdexcept>
#include "asio/detail/socket_types.hpp"
#include "asio/detail/throw_exception.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {
namespace socket_option {

// Helper template for implementing boolean-based options.
template <int Level, int Name>
class boolean
{
public:
  // Default constructor.
  boolean()
    : value_(0)
  {
  }

  // Construct with a specific option value.
  explicit boolean(bool v)
    : value_(v ? 1 : 0)
  {
  }

  // Set the current value of the boolean.
  boolean& operator=(bool v)
  {
    value_ = v ? 1 : 0;
    return *this;
  }

  // Get the current value of the boolean.
  bool value() const
  {
    return !!value_;
  }

  // Convert to bool.
  operator bool() const
  {
    return !!value_;
  }

  // Test for false.
  bool operator!() const
  {
    return !value_;
  }

  // Get the level of the socket option.
  template <typename Protocol>
  int level(const Protocol&) const
  {
    return Level;
  }

  // Get the name of the socket option.
  template <typename Protocol>
  int name(const Protocol&) const
  {
    return Name;
  }

  // Get the address of the boolean data.
  template <typename Protocol>
  int* data(const Protocol&)
  {
    return &value_;
  }

  // Get the address of the boolean data.
  template <typename Protocol>
  const int* data(const Protocol&) const
  {
    return &value_;
  }

  // Get the size of the boolean data.
  template <typename Protocol>
  std::size_t size(const Protocol&) const
  {
    return sizeof(value_);
  }

  // Set the size of the boolean data.
  template <typename Protocol>
  void resize(const Protocol&, std::size_t s)
  {
    // On some platforms (e.g. Windows Vista), the getsockopt function will
    // return the size of a boolean socket option as one byte, even though a
    // four byte integer was passed in.
    switch (s)
    {
    case sizeof(char):
      value_ = *reinterpret_cast<char*>(&value_) ? 1 : 0;
      break;
    case sizeof(value_):
      break;
    default:
      {
        std::length_error ex("boolean socket option resize");
        asio::detail::throw_exception(ex);
      }
    }
  }

private:
  int value_;
};

// Helper template for implementing integer options.
template <int Level, int Name>
class integer
{
public:
  // Default constructor.
  integer()
    : value_(0)
  {
  }

  // Construct with a specific option value.
  explicit integer(int v)
    : value_(v)
  {
  }

  // Set the value of the int option.
  integer& operator=(int v)
  {
    value_ = v;
    return *this;
  }

  // Get the current value of the int option.
  int value() const
  {
    return value_;
  }

  // Get the level of the socket option.
  template <typename Protocol>
  int level(const Protocol&) const
  {
    return Level;
  }

  // Get the name of the socket option.
  template <typename Protocol>
  int name(const Protocol&) const
  {
    return Name;
  }

  // Get the address of the int data.
  template <typename Protocol>
  int* data(const Protocol&)
  {
    return &value_;
  }

  // Get the address of the int data.
  template <typename Protocol>
  const int* data(const Protocol&) const
  {
    return &value_;
  }

  // Get the size of the int data.
  template <typename Protocol>
  std::size_t size(const Protocol&) const
  {
    return sizeof(value_);
  }

  // Set the size of the int data.
  template <typename Protocol>
  void resize(const Protocol&, std::size_t s)
  {
    if (s != sizeof(value_))
    {
      std::length_error ex("integer socket option resize");
      asio::detail::throw_exception(ex);
    }
  }

private:
  int value_;
};

// Helper template for implementing linger options.
template <int Level, int Name>
class linger
{
public:
  // Default constructor.
  linger()
  {
    value_.l_onoff = 0;
    value_.l_linger = 0;
  }

  // Construct with specific option values.
  linger(bool e, int t)
  {
    enabled(e);
    timeout ASIO_PREVENT_MACRO_SUBSTITUTION(t);
  }

  // Set the value for whether linger is enabled.
  void enabled(bool value)
  {
    value_.l_onoff = value ? 1 : 0;
  }

  // Get the value for whether linger is enabled.
  bool enabled() const
  {
    return value_.l_onoff != 0;
  }

  // Set the value for the linger timeout.
  void timeout ASIO_PREVENT_MACRO_SUBSTITUTION(int value)
  {
#if defined(WIN32)
    value_.l_linger = static_cast<u_short>(value);
#else
    value_.l_linger = value;
#endif
  }

  // Get the value for the linger timeout.
  int timeout ASIO_PREVENT_MACRO_SUBSTITUTION() const
  {
    return static_cast<int>(value_.l_linger);
  }

  // Get the level of the socket option.
  template <typename Protocol>
  int level(const Protocol&) const
  {
    return Level;
  }

  // Get the name of the socket option.
  template <typename Protocol>
  int name(const Protocol&) const
  {
    return Name;
  }

  // Get the address of the linger data.
  template <typename Protocol>
  detail::linger_type* data(const Protocol&)
  {
    return &value_;
  }

  // Get the address of the linger data.
  template <typename Protocol>
  const detail::linger_type* data(const Protocol&) const
  {
    return &value_;
  }

  // Get the size of the linger data.
  template <typename Protocol>
  std::size_t size(const Protocol&) const
  {
    return sizeof(value_);
  }

  // Set the size of the int data.
  template <typename Protocol>
  void resize(const Protocol&, std::size_t s)
  {
    if (s != sizeof(value_))
    {
      std::length_error ex("linger socket option resize");
      asio::detail::throw_exception(ex);
    }
  }

private:
  detail::linger_type value_;
};

// Helper template for implementing timeout options.
template <int Level, int Name>
class timeout
{
public:
  // Note that a timeout of zero means "no timeout."
  timeout ASIO_PREVENT_MACRO_SUBSTITUTION()
    : timeout(chrono::seconds(0))
  {
  }

  template <typename Duration>
  explicit timeout ASIO_PREVENT_MACRO_SUBSTITUTION(const Duration& duration)
  {
    *this = duration;
  }

  template <typename Duration>
  timeout& operator=(const Duration& duration)
  {
    // There is an unlikely but possible corner case to consider.
    // If `duration` is positive but smaller than the precision of `value_`,
    // then `duration_cast` will round it down to zero. This is problematic
    // because zero doesn't mean "time out immediately," but instead means
    // "never time out."
    // So, if `duration` is too small, use the smallest positive representable
    // `value_` instead of zero.
  #if defined(ASIO_WINDOWS)
    if (duration > duration.zero() && duration < chrono::milliseconds(1))
      value_ = 1;
    else
      value_ = duration_cast<chrono::milliseconds>(duration).count();
  #else
    if (duration > duration.zero() && duration < chrono::microseconds(1))
    {
      value_.tv_sec = 0;
      value_.tv_usec = 1;
    }
    else
    {
      const auto seconds = duration_cast<chrono::seconds>(duration);
      value_.tv_sec = seconds.count();
      value_.tv_usec =
        duration_cast<chrono::microseconds>(duration - seconds).count();
    }
  #endif
    return *this;
  }

  chrono::microseconds value() const
  {
  #if defined(ASIO_WINDOWS)
    return chrono::milliseconds(value_);
  #else
    return
      chrono::seconds(value_.tv_sec) + chrono::microseconds(value_.tv_usec);
  #endif
  }

  int level() const
  {
    return Level;
  }

  int name() const
  {
    return Name;
  }

  detail::timeout_type* data()
  {
    return &value_;
  }

  const detail::timeout_type* data() const
  {
    return &value_;
  }

  std::size_t size() const
  {
    return sizeof(value_);
  }

  template <typename Protocol>
  int level(const Protocol&) const
  {
    return level();
  }

  template <typename Protocol>
  int name(const Protocol&) const
  {
    return name();
  }

  template <typename Protocol>
  detail::timeout_type* data(const Protocol&)
  {
    return data();
  }

  template <typename Protocol>
  const detail::timeout_type* data(const Protocol&) const
  {
    return data();
  }

  template <typename Protocol>
  std::size_t size(const Protocol&) const
  {
    return size();
  }

  template <typename Protocol>
  void resize(const Protocol&, std::size_t s)
  {
    // Some options can be resized, some can't. Timeouts can't.
    // `socket_ops::getsockopts` will still call `.resize` on us, but the size
    // will always be the size we already had.
    if (s != sizeof(value_))
    {
      std::length_error ex("timeout socket option resize");
      asio::detail::throw_exception(ex);
    }
  }

private:
  detail::timeout_type value_;
};

} // namespace socket_option
} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_SOCKET_OPTION_HPP
