/* Copyright (c) 2002-2004 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE_1_0.txt or http://www.boost.org/LICENSE_1_0.txt)
 * Author: Jeff Garland, Bart Garst
 * $Date: 2008-02-27 12:00:24 -0800 (Wed, 27 Feb 2008) $
 */



#ifndef BOOST_DATE_TIME_SOURCE
#define BOOST_DATE_TIME_SOURCE
#endif
#include "boost/date_time/gregorian/greg_weekday.hpp"

#include "greg_names.hpp"

namespace boost {
namespace gregorian {
  
  //! Return a 3 digit english string of the day of week (eg: Sun)
  const char*
  greg_weekday::as_short_string() const 
  {
    return short_weekday_names[value_];
  }
  //! Return a point to a long english string representing day of week
  const char*
  greg_weekday::as_long_string()  const 
  {
    return long_weekday_names[value_];
  }
  
#ifndef BOOST_NO_STD_WSTRING
  //! Return a 3 digit english wchar_t string of the day of week (eg: Sun)
  const wchar_t*
  greg_weekday::as_short_wstring() const 
  {
    return w_short_weekday_names[value_];
  }
  //! Return a point to a long english wchar_t string representing day of week
  const wchar_t*
  greg_weekday::as_long_wstring()  const 
  {
    return w_long_weekday_names[value_];
  }
#endif // BOOST_NO_STD_WSTRING
  
} } //namespace gregorian

