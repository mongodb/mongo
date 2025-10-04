/*
 * Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <bson/bson-config.h>
#include <bson/bson-compat.h>


#if defined(BSON_HAVE_CLOCK_GETTIME)
#include <time.h>
#include <sys/time.h>
#endif

#include <bson/bson-clock.h>

/*
 *--------------------------------------------------------------------------
 *
 * bson_gettimeofday --
 *
 *       A wrapper around gettimeofday() with fallback support for Windows.
 *
 * Returns:
 *       0 if successful.
 *
 * Side effects:
 *       @tv is set.
 *
 *--------------------------------------------------------------------------
 */

int
bson_gettimeofday (struct timeval *tv) /* OUT */
{
#if defined(_WIN32)
#if defined(_MSC_VER)
#define DELTA_EPOCH_IN_MICROSEC 11644473600000000Ui64
#else
#define DELTA_EPOCH_IN_MICROSEC 11644473600000000ULL
#endif
   FILETIME ft;
   uint64_t tmp = 0;

   /*
    * The const value is shamelessly stolen from
    * http://www.boost.org/doc/libs/1_55_0/boost/chrono/detail/inlined/win/chrono.hpp
    *
    * File times are the number of 100 nanosecond intervals elapsed since
    * 12:00 am Jan 1, 1601 UTC.  I haven't check the math particularly hard
    *
    * ...  good luck
    */

   if (tv) {
      GetSystemTimeAsFileTime (&ft);

      /* pull out of the filetime into a 64 bit uint */
      tmp |= ft.dwHighDateTime;
      tmp <<= 32;
      tmp |= ft.dwLowDateTime;

      /* convert from 100's of nanosecs to microsecs */
      tmp /= 10;

      /* adjust to unix epoch */
      tmp -= DELTA_EPOCH_IN_MICROSEC;

      tv->tv_sec = (long) (tmp / 1000000UL);
      tv->tv_usec = (long) (tmp % 1000000UL);
   }

   return 0;
#else
   return gettimeofday (tv, NULL);
#endif
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_get_monotonic_time --
 *
 *       Returns the monotonic system time, if available. A best effort is
 *       made to use the monotonic clock. However, some systems may not
 *       support such a feature.
 *
 * Returns:
 *       The monotonic clock in microseconds.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

int64_t
bson_get_monotonic_time (void)
{
#if defined(BSON_HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
   struct timespec ts;
   /* ts.tv_sec may be a four-byte integer on 32 bit machines, so cast to
    * int64_t to avoid truncation. */
   clock_gettime (CLOCK_MONOTONIC, &ts);
   return (((int64_t) ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
#elif defined(_WIN32)
   /* Despite it's name, this is in milliseconds! */
   int64_t ticks = GetTickCount64 ();
   return (ticks * 1000);
#elif defined(__hpux__)
   int64_t nanosec = gethrtime ();
   return (nanosec / 1000UL);
#else
#pragma message "Monotonic clock is not yet supported on your platform."
   struct timeval tv;

   bson_gettimeofday (&tv);
   return ((int64_t) tv.tv_sec * 1000000) + tv.tv_usec;
#endif
}
