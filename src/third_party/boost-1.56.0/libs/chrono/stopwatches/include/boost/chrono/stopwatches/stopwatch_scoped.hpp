//  boost/chrono/stopwatches/stopwatch_scoped.hpp  ------------------------------------------------------------//
//  Copyright 2009-2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_STOPWATCH_SCOPED_HPP
#define BOOST_CHRONO_STOPWATCHES_STOPWATCH_SCOPED_HPP

#include <boost/chrono/config.hpp>

#include <boost/chrono/chrono.hpp>
#include <boost/chrono/detail/system.hpp>

namespace boost
{
  namespace chrono
  {

    //--------------------------------------------------------------------------------------//
    template<class Stopwatch>
    class stopwatch_runner
    {
    public:
      typedef Stopwatch stopwatch;
      stopwatch_runner(stopwatch & a) :
        stopwatch_(a)
      {
        stopwatch_.start();
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      stopwatch_runner(stopwatch & a, system::error_code & ec) :
        stopwatch_(a)
      {
        stopwatch_.start(ec);
      }
#endif
      ~stopwatch_runner()
      {
        stopwatch_.stop();
      }
    private:
      stopwatch& stopwatch_;
      stopwatch_runner();//= delete;
      stopwatch_runner(const stopwatch_runner&); // = delete;
      stopwatch_runner& operator=(const stopwatch_runner&); // = delete;

    };

    //--------------------------------------------------------------------------------------//
    template<class Stopwatch>
    class stopwatch_stopper
    {
    public:
      typedef Stopwatch stopwatch;
      stopwatch_stopper(stopwatch & a) :
        stopwatch_(a)
      {
        stopwatch_.stop();
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      stopwatch_stopper(stopwatch & a, system::error_code & ec) :
        stopwatch_(a)
      {
        stopwatch_.stop(ec);
      }
#endif
      ~stopwatch_stopper()
      {
        stopwatch_.start();
      }
    private:
      stopwatch& stopwatch_;
      stopwatch_stopper();//= delete;
      stopwatch_stopper(const stopwatch_stopper&); // = delete;
      stopwatch_stopper& operator=(const stopwatch_stopper&); // = delete;

    };

    //--------------------------------------------------------------------------------------//
    template<class Stopwatch>
    class stopwatch_suspender
    {
    public:
      typedef Stopwatch stopwatch;
      stopwatch_suspender(stopwatch & a) :
        stopwatch_(a)
      {
        stopwatch_.suspend();
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      stopwatch_suspender(stopwatch & a, system::error_code & ec) :
        stopwatch_(a)
      {
        stopwatch_.suspend(ec);
      }
#endif

      ~stopwatch_suspender()
      {
        stopwatch_.resume();
      }
    private:
      stopwatch& stopwatch_;
      stopwatch_suspender(); // = delete;
      stopwatch_suspender(const stopwatch_suspender&); // = delete;
      stopwatch_suspender& operator=(const stopwatch_suspender&); // = delete;
    };

    //--------------------------------------------------------------------------------------//
    template<class Stopwatch>
    class stopwatch_resumer
    {
    public:
      typedef Stopwatch stopwatch;
      stopwatch_resumer(stopwatch & a) :
        stopwatch_(a)
      {
        stopwatch_.resume();
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      stopwatch_resumer(stopwatch & a, system::error_code & ec) :
        stopwatch_(a)
      {
        stopwatch_.resume(ec);
      }
#endif
      ~stopwatch_resumer()
      {
        stopwatch_.suspend();
      }
    private:
      stopwatch& stopwatch_;
      stopwatch_resumer(); // = delete;
      stopwatch_resumer(const stopwatch_resumer&); // = delete;
      stopwatch_resumer& operator=(const stopwatch_resumer&); // = delete;
    };

  } // namespace chrono
} // namespace boost

#endif
