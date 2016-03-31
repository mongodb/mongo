//  boost/chrono/stopwatches/reporters/stopclock.hpp
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or
//   copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_REPORTERS_LAPS_STOPCLOCK_HPP
#define BOOST_CHRONO_STOPWATCHES_REPORTERS_LAPS_STOPCLOCK_HPP

#include <boost/chrono/config.hpp>

#if !defined(BOOST_ENABLE_WARNINGS) && !defined(BOOST_CHRONO_ENABLE_WARNINGS)
#if defined __GNUC__
#pragma GCC system_header
#elif defined __SUNPRO_CC
#pragma disable_warn
#elif defined _MSC_VER
#pragma warning(push, 1)
#endif
#endif

#include <boost/chrono/stopwatches/reporters/laps_accumulator_set_stopwatch_default_formatter.hpp>
#include <boost/chrono/stopwatches/reporters/stopwatch_reporter_default_formatter.hpp>
#include <boost/chrono/stopwatches/reporters/stopwatch_reporter.hpp>
#include <boost/chrono/stopwatches/stopwatch_scoped.hpp>
#include <boost/chrono/stopwatches/stopwatch.hpp>
#include <boost/chrono/stopwatches/dont_start.hpp>
#include <boost/chrono/chrono.hpp>
#include <boost/chrono/detail/system.hpp>
#include <boost/cstdint.hpp>
#include <cassert>

namespace boost
{
  namespace chrono
  {

    template<class CharT, typename Clock, typename LapsCollector, class Formatter>
    class basic_stopclock: public basic_stopwatch_reporter<CharT, stopwatch<Clock, LapsCollector>, Formatter>
    {
    public:
      typedef basic_stopwatch_reporter<CharT, stopwatch<Clock, LapsCollector>, Formatter> base_type;
      typedef Clock clock;
      typedef stopwatch<Clock, LapsCollector> stopwatch_type;
      typedef Formatter formatter_type;

      basic_stopclock()
      {
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit basic_stopclock(system::error_code & ec) :
        base_type(ec)
      {
      }
#endif
      explicit basic_stopclock(
          const dont_start_t& tag
      ) BOOST_NOEXCEPT :
      base_type(tag)
      {
      }

      explicit basic_stopclock(formatter_type const& fmt) :
        base_type(fmt)
      {
      }

      explicit basic_stopclock(const typename Formatter::char_type* fmt) :
        base_type(fmt)
      {
      }
      explicit basic_stopclock(typename Formatter::string_type const& fmt) :
        base_type(fmt)
      {
      }
      typedef stopwatch_runner<basic_stopclock<CharT,Clock, LapsCollector, Formatter> >
          scoped_run;
      typedef stopwatch_stopper<basic_stopclock<CharT,Clock, LapsCollector, Formatter> >
          scoped_stop;
      typedef stopwatch_suspender<basic_stopclock<CharT,Clock, LapsCollector, Formatter> >
          scoped_suspend;
      typedef stopwatch_resumer<basic_stopclock<CharT,Clock, LapsCollector, Formatter> >
          scoped_resume;

    protected:

      basic_stopclock(const basic_stopclock&); // = delete;
      basic_stopclock& operator=(const basic_stopclock&); // = delete;
    };


    template<typename Clock=high_resolution_clock, typename LapsCollector=no_memory<typename Clock::duration>,
        class Formatter = typename basic_stopwatch_reporter_default_formatter<char, stopwatch<Clock, LapsCollector> >::type>
    class stopclock;

    template<class Stopwatch, class Formatter>
    struct basic_stopwatch_reporter_default_formatter<char, stopclock<Stopwatch,
        Formatter> >
    {
      typedef Formatter type;
    };

    template<typename Clock, typename LapsCollector, class Formatter>
    class stopclock: public basic_stopclock<char, Clock, LapsCollector, Formatter>
    {
      typedef basic_stopclock<char, Clock, LapsCollector, Formatter> base_type;
    public:
      typedef Clock clock;
      typedef typename base_type::stopwatch_type stopwatch_type;
      typedef Formatter formatter_type;

      stopclock()
      {
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit stopclock(system::error_code & ec) :
        base_type(ec)
      {
      }
#endif
      explicit stopclock(
          const dont_start_t& tag
      ) BOOST_NOEXCEPT :
      base_type(tag)
      {
      }

      explicit stopclock(formatter_type const& fmt) :
        base_type(fmt)
      {
      }

      explicit stopclock(const typename Formatter::char_type* fmt) :
        base_type(fmt)
      {
      }
      explicit stopclock(typename Formatter::string_type const& fmt) :
        base_type(fmt)
      {
      }
      typedef stopwatch_runner<stopclock<Clock, LapsCollector, Formatter> >
          scoped_run;
      typedef stopwatch_stopper<stopclock<Clock, LapsCollector, Formatter> >
          scoped_stop;
      typedef stopwatch_suspender<stopclock<Clock, LapsCollector, Formatter> >
          scoped_suspend;
      typedef stopwatch_resumer<stopclock<Clock, LapsCollector, Formatter> >
          scoped_resume;

    protected:

      stopclock(const stopclock&); // = delete;
      stopclock& operator=(const stopclock&); // = delete;
    };



  } // namespace chrono
} // namespace boost


#if !defined(BOOST_ENABLE_WARNINGS) && !defined(BOOST_CHRONO_ENABLE_WARNINGS)
#if defined __SUNPRO_CC
#pragma enable_warn
#elif defined _MSC_VER
#pragma warning(pop)
#endif
#endif

#endif


