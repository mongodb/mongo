#ifndef DATE_TIME_C_TIME_HPP___
#define DATE_TIME_C_TIME_HPP___

/* Copyright (c) 2002,2003,2005 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland, Bart Garst 
 * $Date: 2005/10/25 02:42:43 $
 */


/*! @file c_time.hpp
  Provide workarounds related to the ctime header
*/

#include "boost/date_time/compiler_config.hpp"
#include <ctime>
//Work around libraries that don't put time_t and time in namespace std

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::time_t; using ::time; using ::localtime;
                using ::tm;  using ::gmtime; }
#endif // BOOST_NO_STDC_NAMESPACE

//The following is used to support high precision time clocks
#ifdef BOOST_HAS_GETTIMEOFDAY
#include <sys/time.h>
#endif

#ifdef BOOST_HAS_FTIME
#include <time.h>
#endif

namespace boost {
namespace date_time {
  //! Provides a uniform interface to some 'ctime' functions
  /*! Provides a uniform interface to some ctime functions and 
   * their '_r' counterparts. The '_r' functions require a pointer to a 
   * user created std::tm struct whereas the regular functions use a 
   * staticly created struct and return a pointer to that. These wrapper 
   * functions require the user to create a std::tm struct and send in a 
   * pointer to it. A pointer to the user created struct will be returned. */
  struct c_time {
    public:
#if defined(BOOST_DATE_TIME_HAS_REENTRANT_STD_FUNCTIONS)
      //! requires a pointer to a user created std::tm struct
      inline
      static std::tm* localtime(const std::time_t* t, std::tm* result)
      {
        // localtime_r() not in namespace std???
        result = localtime_r(t, result);
        return result;
      }
      //! requires a pointer to a user created std::tm struct
      inline
      static std::tm* gmtime(const std::time_t* t, std::tm* result)
      {
        // gmtime_r() not in namespace std???
        result = gmtime_r(t, result);
        return result;
      }
#else // BOOST_HAS_THREADS

#if (defined(_MSC_VER) && (_MSC_VER >= 1400))
#pragma warning(push) // preserve warning settings
#pragma warning(disable : 4996) // disable depricated localtime/gmtime warning on vc8
#endif // _MSC_VER >= 1400
      //! requires a pointer to a user created std::tm struct
      inline
      static std::tm* localtime(const std::time_t* t, std::tm* result)
      {
        result = std::localtime(t);
        return result;
      }
      //! requires a pointer to a user created std::tm struct
      inline
      static std::tm* gmtime(const std::time_t* t, std::tm* result)
      {
        result = std::gmtime(t);
        return result;
      }
#if (defined(_MSC_VER) && (_MSC_VER >= 1400))
#pragma warning(pop) // restore warnings to previous state
#endif // _MSC_VER >= 1400

#endif // BOOST_HAS_THREADS
  };
}} // namespaces
                
#endif // DATE_TIME_C_TIME_HPP___
