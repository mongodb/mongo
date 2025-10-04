//  This code is based on Timer and Chrono code. Thanks to authors:
//
//  Boost.Timer:
//  Copyright Beman Dawes 1994-2007, 2011
//
//  Boost.Chrono:
//  Copyright Beman Dawes 2008
//  Copyright 2009-2010 Vicente J. Botet Escriba
//
//  Simplified and modified to be able to support exceptionless (-fno-exceptions).
//  Boost.Timer depends on Boost.Chorno wich uses boost::throw_exception.
//  And Boost.Chrono DLLs don't build in Win32 as there is no 
//  boost::throw_exception(std::exception const&) implementation
//  in Boost.Chrono:
//
//  Copyright 2020 Ion Gaztanaga
//
//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//----------------------------------------------------------------------------//
//                                Windows                                     //
//----------------------------------------------------------------------------//
#ifndef BOOST_MOVE_DETAIL_NSEC_CLOCK_HPP
#define BOOST_MOVE_DETAIL_NSEC_CLOCK_HPP

#include <boost/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/detail/workaround.hpp>
#include <cstdlib>


#   if (defined(_WIN32) || defined(__WIN32__) || defined(WIN32))
#     define BOOST_MOVE_DETAIL_WINDOWS_API
#   elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)
#     define BOOST_MOVE_DETAIL_MAC_API
#   else
#     define BOOST_MOVE_DETAIL_POSIX_API
#   endif

#if defined(BOOST_MOVE_DETAIL_WINDOWS_API)

#include <cassert>

#if defined( BOOST_USE_WINDOWS_H )
#include <windows.h>
#else

#if defined (WIN32_PLATFORM_PSPC)
#define BOOST_MOVE_WINAPI_IMPORT BOOST_SYMBOL_IMPORT
#define BOOST_MOVE_WINAPI_IMPORT_EXCEPT_WM
#elif defined (_WIN32_WCE)
#define BOOST_MOVE_WINAPI_IMPORT
#define BOOST_MOVE_WINAPI_IMPORT_EXCEPT_WM
#else
#define BOOST_MOVE_WINAPI_IMPORT BOOST_SYMBOL_IMPORT
#define BOOST_MOVE_WINAPI_IMPORT_EXCEPT_WM BOOST_SYMBOL_IMPORT
#endif

#if defined(WINAPI)
#define BOOST_MOVE_WINAPI_CC WINAPI
#else
   #if defined(_M_IX86) || defined(__i386__)
   #define BOOST_MOVE_WINAPI_CC __stdcall
   #else
   // On architectures other than 32-bit x86 __stdcall is ignored. Clang also issues a warning.
   #define BOOST_MOVE_WINAPI_CC
   #endif
#endif


extern "C" {

union _LARGE_INTEGER;
typedef long long QuadPart;

BOOST_MOVE_WINAPI_IMPORT_EXCEPT_WM int BOOST_MOVE_WINAPI_CC
QueryPerformanceCounter(::_LARGE_INTEGER* lpPerformanceCount);

BOOST_MOVE_WINAPI_IMPORT_EXCEPT_WM int BOOST_MOVE_WINAPI_CC
QueryPerformanceFrequency(::_LARGE_INTEGER* lpFrequency);

} // extern "C"
#endif


namespace boost { namespace move_detail {

BOOST_FORCEINLINE int QueryPerformanceCounter(long long* lpPerformanceCount)
{
    return ::QueryPerformanceCounter(reinterpret_cast< ::_LARGE_INTEGER* >(lpPerformanceCount));
}

BOOST_FORCEINLINE int QueryPerformanceFrequency(long long* lpFrequency)
{
    return ::QueryPerformanceFrequency(reinterpret_cast< ::_LARGE_INTEGER* >(lpFrequency));
}


template<int Dummy>
struct QPFHolder
{
   static inline double get_nsec_per_tic()
   {
      long long freq;
      //According to MS documentation:
      //"On systems that run Windows XP or later, the function will always succeed and will thus never return zero"
      (void)boost::move_detail::QueryPerformanceFrequency(&freq);
      return double(1000000000.0L / double(freq));
   }

   static const double nanosecs_per_tic;
};

template<int Dummy>
const double QPFHolder<Dummy>::nanosecs_per_tic = get_nsec_per_tic();

inline boost::uint64_t nsec_clock() BOOST_NOEXCEPT
{
   double nanosecs_per_tic = QPFHolder<0>::nanosecs_per_tic;
   
   long long pcount;
   //According to MS documentation:
   //"On systems that run Windows XP or later, the function will always succeed and will thus never return zero"
   (void)boost::move_detail::QueryPerformanceCounter( &pcount );
   return static_cast<boost::uint64_t>(nanosecs_per_tic * double(pcount));
}

}}  //namespace boost { namespace move_detail {

#elif defined(BOOST_MOVE_DETAIL_MAC_API)

#include <mach/mach_time.h>  // mach_absolute_time, mach_timebase_info_data_t

inline boost::uint64_t nsec_clock() BOOST_NOEXCEPT
{
   boost::uint64_t count = ::mach_absolute_time();

   mach_timebase_info_data_t info;
   mach_timebase_info(&info);
   return static_cast<boost::uint64_t>
      ( static_cast<double>(count)*(static_cast<double>(info.numer) / info.denom) );
}

#elif defined(BOOST_MOVE_DETAIL_POSIX_API)

#include <time.h>

#  if defined(CLOCK_MONOTONIC_PRECISE)   //BSD
#     define BOOST_MOVE_DETAIL_CLOCK_MONOTONIC CLOCK_MONOTONIC_PRECISE
#  elif defined(CLOCK_MONOTONIC_RAW)     //Linux
#     define BOOST_MOVE_DETAIL_CLOCK_MONOTONIC CLOCK_MONOTONIC_RAW
#  elif defined(CLOCK_HIGHRES)           //Solaris
#     define BOOST_MOVE_DETAIL_CLOCK_MONOTONIC CLOCK_HIGHRES
#  elif defined(CLOCK_MONOTONIC)         //POSIX (AIX, BSD, Linux, Solaris)
#     define BOOST_MOVE_DETAIL_CLOCK_MONOTONIC CLOCK_MONOTONIC
#  else
#     error "No high resolution steady clock in your system, please provide a patch"
#  endif

inline boost::uint64_t nsec_clock() BOOST_NOEXCEPT
{
   struct timespec count;
   ::clock_gettime(BOOST_MOVE_DETAIL_CLOCK_MONOTONIC, &count);
   boost::uint64_t r = static_cast<boost::uint64_t>(count.tv_sec);
   r *= 1000000000U;
   r += static_cast<boost::uint64_t>(count.tv_nsec);
   return r;
}

#endif  // POSIX

namespace boost { namespace move_detail {

typedef boost::uint64_t nanosecond_type;

struct cpu_times
{
   nanosecond_type wall;
   nanosecond_type user;
   nanosecond_type system;

   void clear() { wall = user = system = 0; }

   cpu_times()
   {  this->clear(); }
};


inline void get_cpu_times(boost::move_detail::cpu_times& current)
{
    current.wall = nsec_clock();
}


class cpu_timer
{
   public:

      //  constructor
      cpu_timer() BOOST_NOEXCEPT                                   { start(); }

      //  observers
      bool          is_stopped() const BOOST_NOEXCEPT              { return m_is_stopped; }
      cpu_times     elapsed() const BOOST_NOEXCEPT;  // does not stop()

      //  actions
      void          start() BOOST_NOEXCEPT;
      void          stop() BOOST_NOEXCEPT;
      void          resume() BOOST_NOEXCEPT; 

   private:
      cpu_times     m_times;
      bool          m_is_stopped;
};


//  cpu_timer  ---------------------------------------------------------------------//

inline void cpu_timer::start() BOOST_NOEXCEPT
{
   m_is_stopped = false;
   get_cpu_times(m_times);
}

inline void cpu_timer::stop() BOOST_NOEXCEPT
{
   if (is_stopped())
      return;
   m_is_stopped = true;
      
   cpu_times current;
   get_cpu_times(current);
   m_times.wall = (current.wall - m_times.wall);
   m_times.user = (current.user - m_times.user);
   m_times.system = (current.system - m_times.system);
}

inline cpu_times cpu_timer::elapsed() const BOOST_NOEXCEPT
{
   if (is_stopped())
      return m_times;
   cpu_times current;
   get_cpu_times(current);
   current.wall -= m_times.wall;
   current.user -= m_times.user;
   current.system -= m_times.system;
   return current;
}

inline void cpu_timer::resume() BOOST_NOEXCEPT
{
   if (is_stopped())
   {
      cpu_times current (m_times);
      start();
      m_times.wall   -= current.wall;
      m_times.user   -= current.user;
      m_times.system -= current.system;
   }
}



}  // namespace move_detail
}  // namespace boost

#endif   //BOOST_MOVE_DETAIL_NSEC_CLOCK_HPP
