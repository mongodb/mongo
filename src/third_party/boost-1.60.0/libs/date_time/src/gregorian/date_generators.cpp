/* Copyright (c) 2002,2003 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE_1_0.txt or http://www.boost.org/LICENSE_1_0.txt)
 * Author: Jeff Garland, Bart Garst 
 * $Date$
 */



#ifndef BOOST_DATE_TIME_SOURCE
#define BOOST_DATE_TIME_SOURCE
#endif
#include "boost/date_time/date_generators.hpp"

namespace boost {
namespace date_time {

  const char* const _nth_as_str[] = {"out of range", "first", "second",
    "third", "fourth", "fifth"};
    
  //! Returns nth arg as string. 1 -> "first", 2 -> "second", max is 5.
  BOOST_DATE_TIME_DECL const char* nth_as_str(int ele)
  {
    if(ele >= 1 && ele <= 5) {
      return _nth_as_str[ele];
    } 
    else {
      return _nth_as_str[0];
    }
  }

} } //namespace date_time 





