#ifndef DATE_TIME_LOCAL_TIME_CONVERSION_HPP__
#define DATE_TIME_LOCAL_TIME_CONVERSION_HPP__

/* Copyright (c) 2003-2004 CrystalClear Software, Inc.
 * Subject to the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland, Bart Garst
 * $Date: 2004/10/07 22:28:21 $
 */


#include "boost/date_time/posix_time/conversion.hpp"
#include "boost/date_time/c_time.hpp"
#include "boost/date_time/local_time/local_date_time.hpp"

namespace boost {
namespace local_time {

//! Function that creates a tm struct from a local_date_time
inline
tm to_tm(const local_date_time& lt) {
  tm lt_tm;
  lt_tm = posix_time::to_tm(lt.local_time());
  if(lt.is_dst()){
    lt_tm.tm_isdst = 1;
  }
  else{
    lt_tm.tm_isdst = 0;
  }
  return lt_tm;
}


}} // namespaces
#endif // DATE_TIME_LOCAL_TIME_CONVERSION_HPP__
