//  boost/chrono/stopwatches/reporters/laps_stopwatch_default_formatter.hpp
//  Copyright 2011 Vicente J. Botet Escriba
//  Copyright (c) Microsoft Corporation 2014
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or
//   copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_REPORTERS_LAPS_STOPWATCH_DEFAULT_FORMATTER_HPP
#define BOOST_CHRONO_STOPWATCHES_REPORTERS_LAPS_STOPWATCH_DEFAULT_FORMATTER_HPP

#include <boost/chrono/stopwatches/reporters/stopwatch_reporter_default_formatter.hpp>
#include <boost/chrono/stopwatches/formatters/accumulator_set_formatter.hpp>
#include <boost/chrono/stopwatches/collectors/laps_accumulator_set.hpp>
#include <boost/chrono/stopwatches/laps_stopwatch.hpp>
#if ! BOOST_OS_WINDOWS || BOOST_PLAT_WINDOWS_DESKTOP
#include <boost/chrono/thread_clock.hpp>
#endif

namespace boost
{
  namespace chrono
  {

    template <typename CharT, typename Clock, typename Features, typename Weight>
    struct basic_stopwatch_reporter_default_formatter<CharT, laps_stopwatch<Clock,laps_accumulator_set<typename Clock::duration,Features, Weight> > >
    {
      typedef basic_accumulator_set_formatter<milli,CharT> type;
    };

  } // namespace chrono
} // namespace boost


#endif


