#ifndef GREG_DURATION_HPP___
#define GREG_DURATION_HPP___

/* Copyright (c) 2002,2003 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland, Bart Garst 
 * $Date: 2004/06/30 00:01:20 $
 */

#include "boost/date_time/date_duration.hpp"
#if defined(BOOST_DATE_TIME_OPTIONAL_GREGORIAN_TYPES)
#include "boost/date_time/date_duration_types.hpp"
#endif
#include "boost/date_time/int_adapter.hpp"


namespace boost {
namespace gregorian {


  //!An internal date representation that includes infinities, not a date
  typedef boost::date_time::duration_traits_adapted date_duration_rep;

  //! Durations in days for gregorian system
  /*! \ingroup date_basics
   */
  typedef date_time::date_duration<date_duration_rep> date_duration;

  //! Shorthand for date_duration
  typedef date_duration days;

} } //namespace gregorian



#endif
