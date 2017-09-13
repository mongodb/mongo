//  boost/chrono/stopwatches/simple_stopwatch.hpp  ------------------------------------------------------------//
//  Copyright 2011 Vicente J. Botet Escriba
//  Copyright (c) Microsoft Corporation 2014
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_SIMPLE_STOPWATCH__HPP
#define BOOST_CHRONO_STOPWATCHES_SIMPLE_STOPWATCH__HPP

#include <boost/chrono/config.hpp>

#include <boost/chrono/chrono.hpp>
#include <boost/system/error_code.hpp>
#include <boost/chrono/thread_clock.hpp>
#include <boost/chrono/process_cpu_clocks.hpp>
#include <utility>

namespace boost
{
  namespace chrono
  {

    /**
     * This class provides the simpler stopwatch which is just able to give the elapsed time since its construction.
     */
    template<typename Clock=high_resolution_clock>
    class simple_stopwatch
    {
    public:
      typedef Clock clock;
      typedef typename Clock::duration duration;
      typedef typename Clock::time_point time_point;
      typedef typename Clock::rep rep;
      typedef typename Clock::period period;
      BOOST_STATIC_CONSTEXPR bool is_steady =             Clock::is_steady;


      simple_stopwatch() :
        start_(clock::now())
      {
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit simple_stopwatch(system::error_code & ec) :
        start_(duration::zero())
      {
        time_point tmp = clock::now(ec);
        if (!BOOST_CHRONO_IS_THROWS(ec))
        {
          if (ec)
          {
            return;
          }
        }
        start_ = tmp;
      }
#endif

      ~simple_stopwatch() BOOST_NOEXCEPT
      {
      }

      duration elapsed()
      {
        return clock::now() - start_;
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      duration elapsed(system::error_code & ec) BOOST_NOEXCEPT
      {
        time_point tmp = clock::now(ec);
        if (!BOOST_CHRONO_IS_THROWS(ec))
        {
          if (ec)
            return duration::zero();
        }
        return tmp - start_;
      }
#endif

    private:
      time_point start_;
      simple_stopwatch(const simple_stopwatch&); // = delete;
      simple_stopwatch& operator=(const simple_stopwatch&); // = delete;
    };

    typedef simple_stopwatch<system_clock> system_simple_stopwatch;
#ifdef BOOST_CHRONO_HAS_CLOCK_STEADY
    typedef simple_stopwatch<steady_clock> steady_simple_stopwatch;
#endif
    typedef simple_stopwatch<high_resolution_clock> high_resolution_simple_stopwatch;

#if defined(BOOST_CHRONO_HAS_PROCESS_CLOCKS)
    typedef simple_stopwatch<process_real_cpu_clock> process_real_cpu_simple_stopwatch;
#if ! BOOST_OS_WINDOWS || BOOST_PLAT_WINDOWS_DESKTOP
    typedef simple_stopwatch<process_user_cpu_clock> process_user_cpu_simple_stopwatch;
    typedef simple_stopwatch<process_system_cpu_clock> process_system_cpu_simple_stopwatch;
    typedef simple_stopwatch<process_cpu_clock> process_cpu_simple_stopwatch;
#endif
#endif

#if defined(BOOST_CHRONO_HAS_THREAD_CLOCK)
    typedef simple_stopwatch<thread_clock> thread_simple_stopwatch;
#endif


  } // namespace chrono
} // namespace boost

#endif
