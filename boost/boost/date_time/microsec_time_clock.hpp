#ifndef DATE_TIME_HIGHRES_TIME_CLOCK_HPP___
#define DATE_TIME_HIGHRES_TIME_CLOCK_HPP___

/* Copyright (c) 2002,2003,2005 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland, Bart Garst
 * $Date: 2006/03/18 16:47:55 $
 */


/*! @file microsec_time_clock.hpp
  This file contains a high resolution time clock implementation.
*/

#include <boost/detail/workaround.hpp>
#include "boost/date_time/c_time.hpp"
#include "boost/cstdint.hpp"
#include "boost/shared_ptr.hpp"

#ifdef BOOST_HAS_FTIME
#include <windows.h>
#endif

#ifdef BOOST_DATE_TIME_HAS_HIGH_PRECISION_CLOCK

namespace boost {
namespace date_time {


  //! A clock providing microsecond level resolution
  /*! A high precision clock that measures the local time
   *  at a resolution up to microseconds and adjusts to the
   *  resolution of the time system.  For example, for the
   *  a library configuration with nano second resolution,
   *  the last 3 places of the fractional seconds will always
   *  be 000 since there are 1000 nano-seconds in a micro second.
   */
  template<class time_type>
  class microsec_clock
  {
  public:
    typedef typename time_type::date_type date_type;
    typedef typename time_type::time_duration_type time_duration_type;
    typedef typename time_duration_type::rep_type resolution_traits_type;

    //! return a local time object for the given zone, based on computer clock
    //JKG -- looks like we could rewrite this against universal_time
    template<class time_zone_type>
    static time_type local_time(shared_ptr<time_zone_type> tz_ptr) {
      typedef typename time_type::utc_time_type utc_time_type;
      typedef second_clock<utc_time_type> second_clock;
      // we'll need to know the utc_offset this machine has
      // in order to get a utc_time_type set to utc
      utc_time_type utc_time = second_clock::universal_time();
      time_duration_type utc_offset = second_clock::local_time() - utc_time;
      // use micro clock to get a local time with sub seconds
      // and adjust it to get a true utc time reading with sub seconds
      utc_time = microsec_clock<utc_time_type>::local_time() - utc_offset;
      return time_type(utc_time, tz_ptr);
    }


  private:
    // we want this enum available for both platforms yet still private
    enum TZ_FOR_CREATE { LOCAL, GMT };
    
  public:

#ifdef BOOST_HAS_GETTIMEOFDAY
    //! Return the local time based on computer clock settings
    static time_type local_time() {
      return create_time(LOCAL);
    }

    //! Get the current day in universal date as a ymd_type
    static time_type universal_time()
    {
      return create_time(GMT);
    }

  private:
    static time_type create_time(TZ_FOR_CREATE tz) {
      timeval tv;
      gettimeofday(&tv, 0); //gettimeofday does not support TZ adjust on Linux.
      std::time_t t = tv.tv_sec;
      boost::uint32_t fs = tv.tv_usec;
      std::tm curr, *curr_ptr = 0;
      if (tz == LOCAL) {
        curr_ptr = c_time::localtime(&t, &curr);
      } else {
        curr_ptr = c_time::gmtime(&t, &curr);
      }
      date_type d(curr_ptr->tm_year + 1900,
                  curr_ptr->tm_mon + 1,
                  curr_ptr->tm_mday);
      //The following line will adjusts the fractional second tick in terms
      //of the current time system.  For example, if the time system
      //doesn't support fractional seconds then res_adjust returns 0
      //and all the fractional seconds return 0.
      int adjust = resolution_traits_type::res_adjust()/1000000;

      time_duration_type td(curr_ptr->tm_hour,
                            curr_ptr->tm_min,
                            curr_ptr->tm_sec,
                            fs*adjust);
      return time_type(d,td);

    }
#endif // BOOST_HAS_GETTIMEOFDAY

#ifdef BOOST_HAS_FTIME
    //! Return the local time based on computer clock settings
    static time_type local_time() {
      FILETIME ft;
      #if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3205))
      // Some runtime library implementations expect local times as the norm for ctime.
      FILETIME ft_utc;
      GetSystemTimeAsFileTime(&ft_utc);
      FileTimeToLocalFileTime(&ft_utc,&ft);
      #else
      GetSystemTimeAsFileTime(&ft);
      #endif
      return create_time(ft, LOCAL);
    }
    
    //! Return the UTC time based on computer settings
    static time_type universal_time() {
      FILETIME ft;
      #if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3205))
      // Some runtime library implementations expect local times as the norm for ctime.
      FILETIME ft_utc;
      GetSystemTimeAsFileTime(&ft_utc);
      FileTimeToLocalFileTime(&ft_utc,&ft);
      #else
      GetSystemTimeAsFileTime(&ft);
      #endif
      return create_time(ft, GMT);
    }

  private:
    static time_type create_time(FILETIME& ft, TZ_FOR_CREATE tz) {
      // offset is difference (in 100-nanoseconds) from
      // 1970-Jan-01 to 1601-Jan-01
      boost::uint64_t c1 = 27111902;
      boost::uint64_t c2 = 3577643008UL; // 'UL' removes compiler warnings
      const boost::uint64_t OFFSET = (c1 << 32) + c2;

      boost::uint64_t filetime = ft.dwHighDateTime;
      filetime = filetime << 32;
      filetime += ft.dwLowDateTime;
      filetime -= OFFSET;
      // filetime now holds 100-nanoseconds since 1970-Jan-01

      // microseconds -- static casts supress warnings
      boost::uint32_t sub_sec = static_cast<boost::uint32_t>((filetime % 10000000) / 10);

      std::time_t t = static_cast<time_t>(filetime / 10000000); // seconds since epoch
      
      std::tm curr, *curr_ptr = 0;
      if (tz == LOCAL) {
        curr_ptr = c_time::localtime(&t, &curr);
      }
      else {
        curr_ptr = c_time::gmtime(&t, &curr);
      }
      date_type d(curr_ptr->tm_year + 1900,
                  curr_ptr->tm_mon + 1,
                  curr_ptr->tm_mday);

      //The following line will adjusts the fractional second tick in terms
      //of the current time system.  For example, if the time system
      //doesn't support fractional seconds then res_adjust returns 0
      //and all the fractional seconds return 0.
      int adjust = static_cast<int>(resolution_traits_type::res_adjust()/1000000);

      time_duration_type td(curr_ptr->tm_hour,
                            curr_ptr->tm_min,
                            curr_ptr->tm_sec,
                            sub_sec * adjust);
                            //st.wMilliseconds * adjust);
      return time_type(d,td);

    }
#endif // BOOST_HAS_FTIME
  };


} } //namespace date_time

#endif //BOOST_DATE_TIME_HAS_HIGH_PRECISION_CLOCK


#endif

