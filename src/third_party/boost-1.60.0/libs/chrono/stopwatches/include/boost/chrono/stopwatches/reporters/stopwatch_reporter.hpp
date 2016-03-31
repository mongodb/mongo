//  boost/chrono/stopwatches/stopwatch_reporter.hpp
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or
//   copy at http://www.boost.org/LICENSE_1_0.txt)
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_REPORTERS_STOPWATCH_REPORTER_HPP
#define BOOST_CHRONO_STOPWATCHES_REPORTERS_STOPWATCH_REPORTER_HPP

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
#include <boost/chrono/stopwatches/stopwatch_scoped.hpp>
#include <boost/chrono/stopwatches/dont_start.hpp>
#include <boost/chrono/chrono.hpp>
#include <boost/chrono/detail/system.hpp>
#include <boost/cstdint.hpp>
#include <cassert>

namespace boost
{
  namespace chrono
  {

    template<class CharT, class Stopwatch, class Formatter=basic_stopwatch_reporter_default_formatter<CharT, Stopwatch> >
    class basic_stopwatch_reporter: public Stopwatch
    {
    public:
      typedef Stopwatch base_type;
      typedef typename Stopwatch::clock clock;
      typedef Stopwatch stopwatch_type;
      typedef Formatter formatter_type;

      basic_stopwatch_reporter() BOOST_NOEXCEPT :
        formatter_(), reported_(false)
      {
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit basic_stopwatch_reporter(system::error_code & ec) :
        base_type(ec), formatter_(), reported_(false)
      {
      }
#endif

      explicit basic_stopwatch_reporter(
          const dont_start_t& tag
      ) BOOST_NOEXCEPT :
      base_type(tag),
        formatter_(), reported_(false)
      {
      }

      explicit basic_stopwatch_reporter(const typename Formatter::char_type* fmt) :
        formatter_(fmt), reported_(false)
      {
      }
      explicit basic_stopwatch_reporter(typename Formatter::string_type const& fmt) :
        formatter_(fmt), reported_(false)
      {
      }
      explicit basic_stopwatch_reporter(formatter_type fmt) :
        formatter_(fmt), reported_(false)
      {
      }

      ~basic_stopwatch_reporter() BOOST_NOEXCEPT
      {
        if (!reported())
        {
          this->report();
        }
      }

      inline void report() BOOST_NOEXCEPT
      {
        formatter_(*this);
        reported_ = true;
      }
//      inline void report(system::error_code & ec)
//      {
//        formatter_(*this, ec);
//        reported_ = true;
//      }

      bool reported() const
      {
        return reported_;
      }

      formatter_type& format()
      {
        return formatter_;
      }

    protected:
      formatter_type formatter_;
      bool reported_;

      basic_stopwatch_reporter(const basic_stopwatch_reporter&); // = delete;
      basic_stopwatch_reporter& operator=(const basic_stopwatch_reporter&); // = delete;
    };


    template<class Stopwatch,
        class Formatter = typename basic_stopwatch_reporter_default_formatter<char, Stopwatch>::type>
    class stopwatch_reporter;

    template<class Stopwatch, class Formatter>
    struct basic_stopwatch_reporter_default_formatter<char, stopwatch_reporter<Stopwatch, Formatter> >
    {
      typedef Formatter type;
    };

    template<class Stopwatch, class Formatter>
    class stopwatch_reporter: public basic_stopwatch_reporter<char, Stopwatch,
        Formatter>
    {
      typedef basic_stopwatch_reporter<char, Stopwatch, Formatter> base_type;
    public:
      typedef typename Stopwatch::clock clock;
      typedef Stopwatch stopwatch_type;
      typedef Formatter formatter_type;

      stopwatch_reporter()
      {
      }

#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit stopwatch_reporter(system::error_code & ec) :
        base_type(ec)
      {
      }
#endif
      explicit stopwatch_reporter(
          const dont_start_t& tag
      ) BOOST_NOEXCEPT :
      base_type(tag)
      {
      }

      explicit stopwatch_reporter(formatter_type const& fmt) :
        base_type(fmt)
      {
      }

      explicit stopwatch_reporter(const typename Formatter::char_type* fmt) :
        base_type(fmt)
      {
      }
      explicit stopwatch_reporter(typename Formatter::string_type const& fmt) :
        base_type(fmt)
      {
      }
      typedef stopwatch_runner<stopwatch_reporter<Stopwatch, Formatter> >
          scoped_run;
      typedef stopwatch_stopper<stopwatch_reporter<Stopwatch, Formatter> >
          scoped_stop;
      typedef stopwatch_suspender<stopwatch_reporter<Stopwatch, Formatter> >
          scoped_suspend;
      typedef stopwatch_resumer<stopwatch_reporter<Stopwatch, Formatter> >
          scoped_resume;

    protected:

      stopwatch_reporter(const stopwatch_reporter&); // = delete;
      stopwatch_reporter& operator=(const stopwatch_reporter&); // = delete;
    };

    template<class Stopwatch,
        class Formatter = typename basic_stopwatch_reporter_default_formatter<wchar_t,
            Stopwatch>::type>
    class wstopwatch_reporter;

    template<class Stopwatch, class Formatter>
    struct basic_stopwatch_reporter_default_formatter<wchar_t, wstopwatch_reporter<Stopwatch, Formatter> >
    {
      typedef Formatter type;
    };

    template<class Stopwatch, class Formatter>
    class wstopwatch_reporter: public basic_stopwatch_reporter<wchar_t, Stopwatch, Formatter>
    {
      typedef basic_stopwatch_reporter<wchar_t, Stopwatch, Formatter> base_type;
    public:
      typedef typename Stopwatch::clock clock;
      typedef Stopwatch stopwatch_type;
      typedef Formatter formatter_type;

      wstopwatch_reporter() :
        base_type()
      {
      }
#if !defined BOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING
      explicit wstopwatch_reporter(system::error_code & ec) :
        base_type(ec)
      {
      }
#endif
      explicit wstopwatch_reporter(
          const dont_start_t& tag
      ) BOOST_NOEXCEPT :
      base_type(tag)
      {
      }

      explicit wstopwatch_reporter(formatter_type const& fmt) :
        base_type(fmt)
      {
      }
      explicit wstopwatch_reporter(const typename Formatter::char_type* fmt) :
        base_type(fmt)
      {
      }
      explicit wstopwatch_reporter(typename Formatter::string_type const& fmt) :
        base_type(fmt)
      {
      }
      typedef stopwatch_runner<wstopwatch_reporter<Stopwatch, Formatter> >
          scoped_run;
      typedef stopwatch_stopper<wstopwatch_reporter<Stopwatch, Formatter> >
          scoped_stop;
      typedef stopwatch_suspender<wstopwatch_reporter<Stopwatch, Formatter> >
          scoped_suspend;
      typedef stopwatch_resumer<wstopwatch_reporter<Stopwatch, Formatter> >
          scoped_resume;

    protected:

      wstopwatch_reporter(const wstopwatch_reporter&); // = delete;
      wstopwatch_reporter& operator=(const wstopwatch_reporter&); // = delete;
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


