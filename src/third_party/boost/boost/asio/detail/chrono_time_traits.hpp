//
// detail/chrono_time_traits.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_CHRONO_TIME_TRAITS_HPP
#define BOOST_ASIO_DETAIL_CHRONO_TIME_TRAITS_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/cstdint.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

// Adapts std::chrono clocks for use with a deadline timer.
template <typename Clock, typename WaitTraits>
struct chrono_time_traits
{
  // The clock type.
  typedef Clock clock_type;

  // The duration type of the clock.
  typedef typename clock_type::duration duration_type;

  // The time point type of the clock.
  typedef typename clock_type::time_point time_type;

  // The period of the clock.
  typedef typename duration_type::period period_type;

  // Get the current time.
  static time_type now()
  {
    return clock_type::now();
  }

  // Add a duration to a time.
  static time_type add(const time_type& t, const duration_type& d)
  {
    return t + d;
  }

  // Subtract one time from another.
  static duration_type subtract(const time_type& t1, const time_type& t2)
  {
    return t1 - t2;
  }

  // Test whether one time is less than another.
  static bool less_than(const time_type& t1, const time_type& t2)
  {
    return t1 < t2;
  }

  // Implement just enough of the posix_time::time_duration interface to supply
  // what the timer_queue requires.
  class posix_time_duration
  {
  public:
    explicit posix_time_duration(const duration_type& d)
      : d_(d)
    {
    }

    boost::int64_t ticks() const
    {
      return d_.count();
    }

    boost::int64_t total_seconds() const
    {
      return duration_cast<1, 1>();
    }

    boost::int64_t total_milliseconds() const
    {
      return duration_cast<1, 1000>();
    }

    boost::int64_t total_microseconds() const
    {
      return duration_cast<1, 1000000>();
    }

  private:
    template <boost::int64_t Num, boost::int64_t Den>
    boost::int64_t duration_cast() const
    {
      const boost::int64_t num = period_type::num * Den;
      const boost::int64_t den = period_type::den * Num;

      if (num == 1 && den == 1)
        return ticks();
      else if (num != 1 && den == 1)
        return ticks() * num;
      else if (num == 1 && period_type::den != 1)
        return ticks() / den;
      else
        return ticks() * num / den;
    }

    duration_type d_;
  };

  // Convert to POSIX duration type.
  static posix_time_duration to_posix_duration(const duration_type& d)
  {
    return posix_time_duration(WaitTraits::to_wait_duration(d));
  }
};

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_DETAIL_CHRONO_TIME_TRAITS_HPP
