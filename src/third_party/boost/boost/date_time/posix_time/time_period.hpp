#ifndef POSIX_TIME_PERIOD_HPP___
#define POSIX_TIME_PERIOD_HPP___

/* Copyright (c) 2002,2003 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE_1_0.txt or http://www.boost.org/LICENSE_1_0.txt)
 * Author: Jeff Garland 
 * $Date: 2008-02-27 15:00:24 -0500 (Wed, 27 Feb 2008) $
 */

#include "boost/date_time/period.hpp"
#include "boost/date_time/posix_time/posix_time_duration.hpp"
#include "boost/date_time/posix_time/ptime.hpp"

namespace boost {
namespace posix_time {

  //! Time period type
  /*! \ingroup time_basics
   */
  typedef date_time::period<ptime, time_duration> time_period;


} }//namespace posix_time


#endif

