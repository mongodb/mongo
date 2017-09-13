//  boost/chrono/stopwatches/formatters/base_formatter.hpp  ------------------------------------------------------------//
//  Copyright 2011 Vicente J. Botet Escriba
//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt
//  See http://www.boost.org/libs/chrono/stopwatches for documentation.

#ifndef BOOST_CHRONO_STOPWATCHES_FORMATTERS_BASE_FORMATTER_HPP
#define BOOST_CHRONO_STOPWATCHES_FORMATTERS_BASE_FORMATTER_HPP

#include <boost/chrono/io/duration_style.hpp>
#include <boost/chrono/duration.hpp>
#include <boost/chrono/chrono_io.hpp>
#include <boost/cstdint.hpp>
#include <iostream>
#include <iomanip>

namespace boost
{
  namespace chrono
  {

    template<typename CharT = char, typename Traits = std::char_traits<CharT> >
    class base_formatter
    {
      base_formatter& operator=(base_formatter const& rhs) ;

    public:
      typedef std::basic_ostream<CharT, Traits> ostream_type;

      base_formatter() :
        precision_(3), os_(std::cout), duration_style_(duration_style::symbol)
      {
      }
      base_formatter(ostream_type& os) :
        precision_(3), os_(os), duration_style_(duration_style::symbol)
      {
      }

      void set_precision(std::size_t precision)
      {
        precision_ = precision;
        if (precision_ > 9)
          precision_ = 9; // sanity check
      }
      void set_os(ostream_type& os)
      {
        os_ = os;
      }
      void set_duration_style(duration_style style)
      {
        duration_style_ = style;
      }

    protected:
      std::size_t precision_;
      ostream_type & os_;
      duration_style duration_style_;

    };

  } // namespace chrono
} // namespace boost


#endif
