//  boost/chrono/stopwatches/formatters/times_formatter.hpp  ------------------------------------------------------------//
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_FORMATTERS_TIMES_HPP
#define BOOST_CHRONO_STOPWATCHES_FORMATTERS_TIMES_HPP

#include <boost/chrono/stopwatches/formatters/base_formatter.hpp>
//#include <boost/chrono/detail/system.hpp>
#include <boost/current_function.hpp>
//#include <boost/chrono/stopwatches/detail/adaptive_string.hpp>
#include <boost/format.hpp>
#include <boost/format/group.hpp>
#include <boost/cstdint.hpp>
#include <boost/assert.hpp>
#include <string>
#include <iostream>
#include <cassert>
#include <iomanip>

#define BOOST_CHRONO_STOPWATCHES_TIMES_FORMAT_DEFAULT "real %1%, cpu %4% (%5%%%), user %2%, system %3%\n"

namespace boost
{
  namespace chrono
  {

    template<typename Ratio = milli, typename CharT = char,
        typename Traits = std::char_traits<CharT>,
        class Alloc = std::allocator<CharT> >
    class basic_times_formatter: public base_formatter<CharT, Traits>, public basic_format<CharT, Traits>
    {

    public:
      typedef base_formatter<CharT, Traits> base_type;
      typedef basic_format<CharT, Traits> format_type;
      typedef std::basic_string<CharT, Traits, Alloc> string_type;
      typedef CharT char_type;
      typedef std::basic_ostream<CharT, Traits> ostream_type;

      basic_times_formatter() :
        base_type(),
        format_type(BOOST_CHRONO_STOPWATCHES_TIMES_FORMAT_DEFAULT)
      {
      }
      basic_times_formatter(ostream_type& os) :
        base_type(os),
        format_type(BOOST_CHRONO_STOPWATCHES_TIMES_FORMAT_DEFAULT)
      {
      }
      basic_times_formatter(const char* fmt, ostream_type& os = std::cout) :
        base_type(os), format_type(fmt)
      {
      }
      basic_times_formatter(string_type const& fmt, ostream_type& os =
          std::cout) :
        base_type(os), format_type(fmt)
      {
      }

//      static string_type format(const char* s)
//      {
//        string_type res(s);
//        res += boost::chrono::detail::adaptive_string(" : ");
//        res += BOOST_CHRONO_STOPWATCHES_TIMES_FORMAT_DEFAULT;
//        return res;
//      }

      template<class Stopwatch>
      void operator()(Stopwatch & stopwatch_
          //, system::error_code & ec= BOOST_CHRONO_THROWS
          )
      {
        typedef typename Stopwatch::duration::rep times_type;
        //times_type times = stopwatch_.elapsed(ec).count();
        times_type times = stopwatch_.elapsed().count();

        if (times.real < 0)
          return;
        double p;
        if (times.real > 0)
          p=double(times.user+times.system)*100/times.real;
        else
          p=0;

        duration_style_io_saver dsios(this->os_);
        this->os_ << static_cast<format_type&>(*this)
            % io::group(std::fixed, std::setprecision(this->precision_), duration_fmt(this->duration_style_), boost::chrono::duration<double, Ratio>(nanoseconds(times.real)))
            % io::group(std::fixed, std::setprecision(this->precision_), duration_fmt(this->duration_style_), boost::chrono::duration<double, Ratio>(nanoseconds(times.user)))
            % io::group(std::fixed, std::setprecision(this->precision_), duration_fmt(this->duration_style_), boost::chrono::duration<double, Ratio>(nanoseconds(times.system)))
            % io::group(std::fixed, std::setprecision(this->precision_), duration_fmt(this->duration_style_), boost::chrono::duration<double, Ratio>(nanoseconds(times.user+times.system)))
            % io::group(std::fixed, std::setprecision(2), p)
        ;
      }
    };

    typedef basic_times_formatter<milli, char> times_formatter;
    typedef basic_times_formatter<milli, wchar_t> wtimes_formatter;

  } // namespace chrono
} // namespace boost

#if 0
#define BOOST_CHRONO_STOPWATCHES_TIMES_FORMAT(F) \
  boost::chrono::detail::adaptive_string(F " : " BOOST_CHRONO_STOPWATCHES_TIMES_FORMAT_DEFAULT)
#ifdef __GNUC__
#define BOOST_CHRONO_STOPWATCHES_TIMES_FUNCTION_FORMAT \
  boost::chrono::times_formatter::format(BOOST_CURRENT_FUNCTION)
#else
#define BOOST_CHRONO_STOPWATCHES_TIMES_FUNCTION_FORMAT \
    BOOST_CHRONO_STOPWATCHES_TIMES_FORMAT(BOOST_CURRENT_FUNCTION)
#endif
#endif

#endif
