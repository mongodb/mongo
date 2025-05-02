//  Copyright (c) 2005-2010 Hartmut Kaiser
//  Copyright (c) 2009      Edward Grace
// 
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HIGH_RESOLUTION_TIMER_MAR_24_2008_1222PM)
#define HIGH_RESOLUTION_TIMER_MAR_24_2008_1222PM

#include <boost/config.hpp>
#include <boost/throw_exception.hpp>

#if defined(BOOST_HAS_UNISTD_H)
#include <unistd.h>
#endif
#include <time.h>
#include <stdexcept>
#include <limits>

#if defined(BOOST_WINDOWS)

#include <windows.h>

namespace util 
{
    ///////////////////////////////////////////////////////////////////////////////
    //
    //  high_resolution_timer 
    //      A timer object measures elapsed time.
    //      CAUTION: Windows only!
    //
    ///////////////////////////////////////////////////////////////////////////////
    class high_resolution_timer
    {
    public:
        high_resolution_timer() 
        {
            restart(); 
        } 

        high_resolution_timer(double t) 
        {
            LARGE_INTEGER frequency;
            if (!QueryPerformanceFrequency(&frequency))
                boost::throw_exception(std::runtime_error("Couldn't acquire frequency"));

            start_time.QuadPart = (LONGLONG)(t * frequency.QuadPart); 
        } 

        high_resolution_timer(high_resolution_timer const& rhs) 
          : start_time(rhs.start_time)
        {
        } 

        static double now()
        {
            SYSTEMTIME st;
            GetSystemTime(&st);

            FILETIME ft;
            SystemTimeToFileTime(&st, &ft);

            LARGE_INTEGER now;
            now.LowPart = ft.dwLowDateTime;
            now.HighPart = ft.dwHighDateTime;

            // FileTime is in 100ns increments, result needs to be in [s]
            return now.QuadPart * 1e-7;
        }

        void restart() 
        { 
            if (!QueryPerformanceCounter(&start_time))
                boost::throw_exception(std::runtime_error("Couldn't initialize start_time"));
        } 
        double elapsed() const                  // return elapsed time in seconds
        { 
            LARGE_INTEGER now;
            if (!QueryPerformanceCounter(&now))
                boost::throw_exception(std::runtime_error("Couldn't get current time"));

            LARGE_INTEGER frequency;
            if (!QueryPerformanceFrequency(&frequency))
                boost::throw_exception(std::runtime_error("Couldn't acquire frequency"));

            return double(now.QuadPart - start_time.QuadPart) / frequency.QuadPart;
        }

        double elapsed_max() const   // return estimated maximum value for elapsed()
        {
            LARGE_INTEGER frequency;
            if (!QueryPerformanceFrequency(&frequency))
                boost::throw_exception(std::runtime_error("Couldn't acquire frequency"));

            return double((std::numeric_limits<LONGLONG>::max)() - start_time.QuadPart) / 
                double(frequency.QuadPart); 
        }

        double elapsed_min() const            // return minimum value for elapsed()
        { 
            LARGE_INTEGER frequency;
            if (!QueryPerformanceFrequency(&frequency))
                boost::throw_exception(std::runtime_error("Couldn't acquire frequency"));

            return 1.0 / frequency.QuadPart; 
        }

    private:
        LARGE_INTEGER start_time;
    }; 

} // namespace util

#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(_POSIX_THREAD_CPUTIME)

#if _POSIX_THREAD_CPUTIME > 0   // timer always supported

namespace util
{

    ///////////////////////////////////////////////////////////////////////////////
    //
    //  high_resolution_timer 
    //      A timer object measures elapsed time.
    //
    ///////////////////////////////////////////////////////////////////////////////
    class high_resolution_timer
    {
    public:
        high_resolution_timer() 
        {
            start_time.tv_sec = 0;
            start_time.tv_nsec = 0;

            restart(); 
        } 

        high_resolution_timer(double t) 
        {
            start_time.tv_sec = time_t(t);
            start_time.tv_nsec = (t - start_time.tv_sec) * 1e9;
        }

        high_resolution_timer(high_resolution_timer const& rhs) 
          : start_time(rhs.start_time)
        {
        } 

        static double now()
        {
            timespec now;
            if (-1 == clock_gettime(CLOCK_REALTIME, &now))
                boost::throw_exception(std::runtime_error("Couldn't get current time"));
            return double(now.tv_sec) + double(now.tv_nsec) * 1e-9;
        }

        void restart() 
        { 
            if (-1 == clock_gettime(CLOCK_REALTIME, &start_time))
                boost::throw_exception(std::runtime_error("Couldn't initialize start_time"));
        } 
        double elapsed() const                  // return elapsed time in seconds
        { 
            timespec now;
            if (-1 == clock_gettime(CLOCK_REALTIME, &now))
                boost::throw_exception(std::runtime_error("Couldn't get current time"));

            if (now.tv_sec == start_time.tv_sec)
                return double(now.tv_nsec - start_time.tv_nsec) * 1e-9;

            return double(now.tv_sec - start_time.tv_sec) + 
                (double(now.tv_nsec - start_time.tv_nsec) * 1e-9);
        }

        double elapsed_max() const   // return estimated maximum value for elapsed()
        {
            return double((std::numeric_limits<time_t>::max)() - start_time.tv_sec); 
        }

        double elapsed_min() const            // return minimum value for elapsed()
        { 
            timespec resolution;
            if (-1 == clock_getres(CLOCK_REALTIME, &resolution))
                boost::throw_exception(std::runtime_error("Couldn't get resolution"));
            return double(resolution.tv_sec + resolution.tv_nsec * 1e-9); 
        }

    private:
        timespec start_time;
    }; 

} // namespace util

#else   // _POSIX_THREAD_CPUTIME > 0

#include <boost/timer.hpp>

// availability of high performance timers must be checked at runtime
namespace util
{
    ///////////////////////////////////////////////////////////////////////////////
    //
    //  high_resolution_timer 
    //      A timer object measures elapsed time.
    //
    ///////////////////////////////////////////////////////////////////////////////
    class high_resolution_timer
    {
    public:
        high_resolution_timer() 
          : use_backup(sysconf(_SC_THREAD_CPUTIME) <= 0)
        {
            if (!use_backup) {
                start_time.tv_sec = 0;
                start_time.tv_nsec = 0;
            }
            restart(); 
        } 

        high_resolution_timer(double t) 
          : use_backup(sysconf(_SC_THREAD_CPUTIME) <= 0)
        {
            if (!use_backup) {
                start_time.tv_sec = time_t(t);
                start_time.tv_nsec = (t - start_time.tv_sec) * 1e9;
            }
        }
        
        high_resolution_timer(high_resolution_timer const& rhs) 
          : use_backup(sysconf(_SC_THREAD_CPUTIME) <= 0),
            start_time(rhs.start_time)
        {
        } 

        static double now()
        {
            if (sysconf(_SC_THREAD_CPUTIME) <= 0)
                return double(std::clock());

            timespec now;
            if (-1 == clock_gettime(CLOCK_REALTIME, &now))
                boost::throw_exception(std::runtime_error("Couldn't get current time"));
            return double(now.tv_sec) + double(now.tv_nsec) * 1e-9;
        }

        void restart() 
        { 
            if (use_backup)
                start_time_backup.restart();
            else if (-1 == clock_gettime(CLOCK_REALTIME, &start_time))
                boost::throw_exception(std::runtime_error("Couldn't initialize start_time"));
        } 
        double elapsed() const                  // return elapsed time in seconds
        { 
            if (use_backup)
                return start_time_backup.elapsed();

            timespec now;
            if (-1 == clock_gettime(CLOCK_REALTIME, &now))
                boost::throw_exception(std::runtime_error("Couldn't get current time"));

            if (now.tv_sec == start_time.tv_sec)
                return double(now.tv_nsec - start_time.tv_nsec) * 1e-9;
                
            return double(now.tv_sec - start_time.tv_sec) + 
                (double(now.tv_nsec - start_time.tv_nsec) * 1e-9);
        }

        double elapsed_max() const   // return estimated maximum value for elapsed()
        {
            if (use_backup)
                start_time_backup.elapsed_max();

            return double((std::numeric_limits<time_t>::max)() - start_time.tv_sec); 
        }

        double elapsed_min() const            // return minimum value for elapsed()
        { 
            if (use_backup)
                start_time_backup.elapsed_min();

            timespec resolution;
            if (-1 == clock_getres(CLOCK_REALTIME, &resolution))
                boost::throw_exception(std::runtime_error("Couldn't get resolution"));
            return double(resolution.tv_sec + resolution.tv_nsec * 1e-9); 
        }

    private:
        bool use_backup;
        timespec start_time;
        boost::timer start_time_backup;
    }; 

} // namespace util

#endif  // _POSIX_THREAD_CPUTIME > 0

#else   //  !defined(BOOST_WINDOWS) && (!defined(_POSIX_TIMERS)
        //      || _POSIX_TIMERS <= 0
        //      || !defined(_POSIX_THREAD_CPUTIME)
        //      || _POSIX_THREAD_CPUTIME <= 0)

#if defined(BOOST_HAS_GETTIMEOFDAY)

// For platforms that do not support _POSIX_TIMERS but do have
// GETTIMEOFDAY, which is still preferable to std::clock()
#include <sys/time.h>

namespace util
{

    ///////////////////////////////////////////////////////////////////////////
    //
    //  high_resolution_timer 
    //      A timer object measures elapsed time.
    //
    //  Implemented with gettimeofday() for platforms that support it,
    //  such as Darwin (OS X) but do not support the previous options.
    //
    //  Copyright (c) 2009 Edward Grace
    //
    ///////////////////////////////////////////////////////////////////////////
    class high_resolution_timer
    {
    private:
        template <typename U>
        static inline double unsigned_diff(const U &a, const U &b)
        {
            if (a > b)
                return static_cast<double>(a-b);
            return -static_cast<double>(b-a);
        }

        // @brief Return the difference between two timeval types.
        // 
        // @param t1 The most recent timeval.
        // @param t0 The historical timeval.
        // 
        // @return The difference between the two in seconds.
        double elapsed(const timeval &t1, const timeval &t0) const
        { 
            if (t1.tv_sec == t0.tv_sec)
                return unsigned_diff(t1.tv_usec,t0.tv_usec) * 1e-6;

            // We do it this way as the result of the difference of the
            // microseconds can be negative if the clock is implemented so
            // that the seconds timer increases in large steps.
            //
            // Naive subtraction of the unsigned types and conversion to
            // double can wreak havoc!
            return unsigned_diff(t1.tv_sec,t0.tv_sec) + 
                unsigned_diff(t1.tv_usec,t0.tv_usec) * 1e-6; 
        }

    public:
        high_resolution_timer() 
        {
            start_time.tv_sec = 0;
            start_time.tv_usec = 0;

            restart(); 
        } 

        high_resolution_timer(double t) 
        {
            start_time.tv_sec = time_t(t);
            start_time.tv_usec = (t - start_time.tv_sec) * 1e6;
        }

        high_resolution_timer(high_resolution_timer const& rhs) 
          : start_time(rhs.start_time)
        {
        } 

        static double now()
        {
            // Under some implementations gettimeofday() will always
            // return zero. If it returns anything else however then
            // we accept this as evidence of an error.  Note we are
            // not assuming that -1 explicitly indicates the error
            // condition, just that non zero is indicative of the
            // error.
            timeval now;
            if (gettimeofday(&now, NULL))
                boost::throw_exception(std::runtime_error("Couldn't get current time"));
            return double(now.tv_sec) + double(now.tv_usec) * 1e-6;
        }

        void restart() 
        { 
            if (gettimeofday(&start_time, NULL))
                boost::throw_exception(std::runtime_error("Couldn't initialize start_time"));
        } 

        double elapsed() const                  // return elapsed time in seconds
        { 
            timeval now;
            if (gettimeofday(&now, NULL))
                boost::throw_exception(std::runtime_error("Couldn't get current time"));
            return elapsed(now,start_time);
        }

        double elapsed_max() const   // return estimated maximum value for elapsed()
        {
            return double((std::numeric_limits<time_t>::max)() - start_time.tv_sec); 
        }

        double elapsed_min() const            // return minimum value for elapsed()
        { 
            // On systems without an explicit clock_getres or similar
            // we can only estimate an upper bound on the resolution
            // by repeatedly calling the gettimeofday function.  This
            // is often likely to be indicative of the true
            // resolution.
            timeval t0, t1;
            double delta(0);

            if (gettimeofday(&t0, NULL)) 
                boost::throw_exception(std::runtime_error("Couldn't get resolution."));

            // Spin around in a tight loop until we observe a change
            // in the reported timer value.
            do {
                if (gettimeofday(&t1, NULL)) 
                    boost::throw_exception(std::runtime_error("Couldn't get resolution."));
                delta = elapsed(t1, t0);
            } while (delta <= 0.0);

            return delta;
        }

    private:
        timeval start_time;
    }; 

} 

#else // BOOST_HAS_GETTIMEOFDAY

//  For platforms other than Windows or Linux, or not implementing gettimeofday
//  simply fall back to boost::timer
#include <boost/timer.hpp>

namespace util
{
    struct high_resolution_timer
        : boost::timer
    {
        static double now()
        {
            return double(std::clock());
        }
    };
}

#endif

#endif

#endif  // HIGH_RESOLUTION_TIMER_AUG_14_2009_0425PM

//
// $Log: high_resolution_timer.hpp,v $
// Revision 1.4  2009/08/14 15:28:10  graceej
// * It is entirely possible for the updating clock to increment the
// * seconds and *decrement* the microseconds field.  Consequently
// * when subtracting these unsigned microseconds fields a wrap-around
// * error can occur.  For this reason elapsed(t1, t0) is used in a
// * similar maner to cycle.h this preserves the sign of the
// * difference.
//


