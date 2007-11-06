#ifndef _GREGORIAN__CONVERSION_HPP___
#define _GREGORIAN__CONVERSION_HPP___

/* Copyright (c) 2004-2005 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland, Bart Garst
 * $Date: 2005/10/25 03:38:28 $
 */

#include <exception>
#include "boost/date_time/gregorian/gregorian_types.hpp"
#include "boost/date_time/c_time.hpp"
#if defined(USE_DATE_TIME_PRE_1_33_FACET_IO)
#  if defined(BOOST_DATE_TIME_INCLUDE_LIMITED_HEADERS)
#    include "boost/date_time/gregorian/formatters_limited.hpp"
#  else
#    include "boost/date_time/gregorian/formatters.hpp"
#  endif // BOOST_DATE_TIME_INCLUDE_LIMITED_HEADERS
#else
#  include <sstream>
#  include "boost/date_time/gregorian/gregorian_io.hpp"
#endif // USE_DATE_TIME_PRE_1_33_FACET_IO

namespace boost {

namespace gregorian {


  //! Converts a date to a tm struct. Throws out_of_range exception if date is a special value
  inline
  std::tm to_tm(const date& d) 
  {
    if(d.is_pos_infinity() || d.is_neg_infinity() || d.is_not_a_date()){
#if defined(USE_DATE_TIME_PRE_1_33_FACET_IO)
      std::string s("tm unable to handle date value of " + to_simple_string(d));
      throw std::out_of_range(s);
#else
      std::stringstream ss;
      ss << "tm unable to handle date value of " << d;
      throw std::out_of_range(ss.str());
#endif // USE_DATE_TIME_PRE_1_33_FACET_IO
    }
    std::tm datetm;
    boost::gregorian::date::ymd_type ymd = d.year_month_day();
    datetm.tm_year = ymd.year-1900; 
    datetm.tm_mon = ymd.month-1; 
    datetm.tm_mday = ymd.day;
    datetm.tm_wday = d.day_of_week();
    datetm.tm_yday = d.day_of_year()-1;
    datetm.tm_hour = datetm.tm_min = datetm.tm_sec = 0;
    datetm.tm_isdst = -1; // negative because not enough info to set tm_isdst
    return datetm;
  }

  //! Converts a tm structure into a date dropping the any time values.
  inline
  date date_from_tm(const std::tm& datetm) 
  {
    return date(static_cast<unsigned short>(datetm.tm_year+1900), 
                static_cast<unsigned short>(datetm.tm_mon+1), 
                static_cast<unsigned short>(datetm.tm_mday));
  }
  

} } //namespace boost::gregorian




#endif

