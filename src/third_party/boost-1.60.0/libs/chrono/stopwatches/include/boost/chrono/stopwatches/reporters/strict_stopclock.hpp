//  boost/chrono/stopwatches/reporters/strict_stopclock.hpp
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or
//   copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_REPORTERS_STRICT_STOPCLOCK_HPP
#define BOOST_CHRONO_STOPWATCHES_REPORTERS_STRICT_STOPCLOCK_HPP

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

#include <boost/chrono/stopwatches/reporters/stopwatch_reporter_default_formatter.hpp>
#include <boost/chrono/stopwatches/reporters/stopwatch_reporter.hpp>
#include <boost/chrono/stopwatches/strict_stopwatch.hpp>
#include <boost/chrono/chrono.hpp>
#include <boost/chrono/detail/system.hpp>
#include <boost/cstdint.hpp>
#include <cassert>

namespace boost
{
  namespace chrono
  {

    template<class CharT, typename Clock, class Formatter>
    class basic_strict_stopclock: public basic_stopwatch_reporter<CharT, strict_stopwatch<Clock>, Formatter>
    {
    public:
      typedef basic_stopwatch_reporter<CharT, strict_stopwatch<Clock>, Formatter> base_type;
      typedef Clock clock;
      typedef strict_stopwatch<Clock> stopwatch;
      typedef Formatter formatter_type;

      basic_strict_stopclock()
      {
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit basic_strict_stopclock(system::error_code & ec) :
        base_type(ec)
      {
      }
#endif

      explicit basic_strict_stopclock(formatter_type const& fmt) :
        base_type(fmt)
      {
      }

      explicit basic_strict_stopclock(const typename Formatter::char_type* fmt) :
        base_type(fmt)
      {
      }
      explicit basic_strict_stopclock(typename Formatter::string_type const& fmt) :
        base_type(fmt)
      {
      }

    protected:

      basic_strict_stopclock(const basic_strict_stopclock&); // = delete;
      basic_strict_stopclock& operator=(const basic_strict_stopclock&); // = delete;
    };


    template<typename Clock=high_resolution_clock,
        class Formatter = typename basic_stopwatch_reporter_default_formatter<char, strict_stopwatch<Clock> >::type>
    class strict_stopclock;

    template<class Stopwatch, class Formatter>
    struct basic_stopwatch_reporter_default_formatter<char, strict_stopclock<Stopwatch, Formatter> >
    {
      typedef Formatter type;
    };

    template<typename Clock, class Formatter>
    class strict_stopclock: public basic_strict_stopclock<char, Clock, Formatter>
    {
      typedef basic_strict_stopclock<char, Clock, Formatter> base_type;
    public:
      typedef Clock clock;
      typedef typename base_type::stopwatch stopwatch;
      typedef Formatter formatter_type;

      strict_stopclock()
      {
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit strict_stopclock(system::error_code & ec) :
        base_type(ec)
      {
      }
#endif

      explicit strict_stopclock(formatter_type const& fmt) :
        base_type(fmt)
      {
      }

      explicit strict_stopclock(const typename Formatter::char_type* fmt) :
        base_type(fmt)
      {
      }
      explicit strict_stopclock(typename Formatter::string_type const& fmt) :
        base_type(fmt)
      {
      }

    protected:

      strict_stopclock(const strict_stopclock&); // = delete;
      strict_stopclock& operator=(const strict_stopclock&); // = delete;
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


