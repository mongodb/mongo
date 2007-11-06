#ifndef DATE_TIME_FILETIME_FUNCTIONS_HPP__
#define DATE_TIME_FILETIME_FUNCTIONS_HPP__

/* Copyright (c) 2004 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland, Bart Garst
 * $Date: 2004/08/04 22:06:05 $
 */

/*! @file filetime_functions.hpp
 * Function(s) for converting between a FILETIME structure and a 
 * time object. This file is only available on systems that have
 * BOOST_HAS_FTIME defined.
 */

#include <boost/date_time/compiler_config.hpp>
#if defined(BOOST_HAS_FTIME) // skip this file if no FILETIME
#include <windows.h>
#include <boost/cstdint.hpp>
#include <boost/date_time/time.hpp>


namespace boost {
namespace date_time {


  //! Create a time object from an initialized FILETIME struct.
  /*! Create a time object from an initialized FILETIME struct.
   * A FILETIME struct holds 100-nanosecond units (0.0000001). When 
   * built with microsecond resolution the FILETIME's sub second value 
   * will be truncated. Nanosecond resolution has no truncation. */
  template<class time_type>
  inline
  time_type time_from_ftime(const FILETIME& ft){
    typedef typename time_type::date_type date_type;
    typedef typename time_type::date_duration_type date_duration_type;
    typedef typename time_type::time_duration_type time_duration_type;

    /* OFFSET is difference between 1970-Jan-01 & 1601-Jan-01 
     * in 100-nanosecond intervals */
    uint64_t c1 = 27111902UL; 
    uint64_t c2 = 3577643008UL; // issues warning without 'UL'
    const uint64_t OFFSET = (c1 << 32) + c2;
    const long sec_pr_day = 86400; // seconds per day

    uint64_t filetime = ft.dwHighDateTime;
    filetime <<= 32;
    filetime += ft.dwLowDateTime;
    filetime -= OFFSET; // filetime is now 100-nanos since 1970-Jan-01

    uint64_t sec = filetime / 10000000;
#if defined(BOOST_DATE_TIME_POSIX_TIME_STD_CONFIG)
    uint64_t sub_sec = (filetime % 10000000) * 100; // nanoseconds
#else
    uint64_t sub_sec = (filetime % 10000000) / 10; // truncate to microseconds
#endif

    // split sec into usable chunks: days, hours, minutes, & seconds
    long _d = sec / sec_pr_day;
    long tmp = sec % sec_pr_day;
    long _h = tmp / 3600; // sec_pr_hour
    tmp %= 3600;
    long _m = tmp / 60; // sec_pr_min
    tmp %= 60;
    long _s = tmp; // seconds

    date_duration_type dd(_d);
    date_type d = date_type(1970, Jan, 01) + dd;
    return time_type(d, time_duration_type(_h, _m, _s, sub_sec));
  }

}} // boost::date_time

#endif // BOOST_HAS_FTIME

#endif // DATE_TIME_FILETIME_FUNCTIONS_HPP__
