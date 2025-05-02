#ifndef BOOST_CORE_DETAIL_SP_THREAD_SLEEP_HPP_INCLUDED
#define BOOST_CORE_DETAIL_SP_THREAD_SLEEP_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

// boost/core/detail/sp_thread_sleep.hpp
//
// inline void bost::core::sp_thread_sleep();
//
//   Cease execution for a while to yield to other threads,
//   as if by calling nanosleep() with an appropriate interval.
//
// Copyright 2008, 2020, 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/config.hpp>
#include <boost/config/pragma_message.hpp>

#if defined( _WIN32 ) || defined( __WIN32__ ) || defined( __CYGWIN__ )

#if defined(BOOST_SP_REPORT_IMPLEMENTATION)
  BOOST_PRAGMA_MESSAGE("Using Sleep(1) in sp_thread_sleep")
#endif

#include <boost/core/detail/sp_win32_sleep.hpp>

namespace boost
{
namespace core
{
namespace detail
{

inline void sp_thread_sleep() BOOST_NOEXCEPT
{
    Sleep( 1 );
}

} // namespace detail

using boost::core::detail::sp_thread_sleep;

} // namespace core
} // namespace boost

#elif defined(BOOST_HAS_NANOSLEEP)

#if defined(BOOST_SP_REPORT_IMPLEMENTATION)
  BOOST_PRAGMA_MESSAGE("Using nanosleep() in sp_thread_sleep")
#endif

#include <time.h>

#if defined(BOOST_HAS_PTHREADS) && !defined(__ANDROID__)
# include <pthread.h>
#endif

namespace boost
{
namespace core
{

inline void sp_thread_sleep() BOOST_NOEXCEPT
{
#if defined(BOOST_HAS_PTHREADS) && !defined(__ANDROID__) && !defined(__OHOS__)

    int oldst;
    pthread_setcancelstate( PTHREAD_CANCEL_DISABLE, &oldst );

#endif

    // g++ -Wextra warns on {} or {0}
    struct timespec rqtp = { 0, 0 };

    // POSIX says that timespec has tv_sec and tv_nsec
    // But it doesn't guarantee order or placement

    rqtp.tv_sec = 0;
    rqtp.tv_nsec = 1000;

    nanosleep( &rqtp, 0 );

#if defined(BOOST_HAS_PTHREADS) && !defined(__ANDROID__) && !defined(__OHOS__)

    pthread_setcancelstate( oldst, &oldst );

#endif

}

} // namespace core
} // namespace boost

#else

#if defined(BOOST_SP_REPORT_IMPLEMENTATION)
  BOOST_PRAGMA_MESSAGE("Using sp_thread_yield() in sp_thread_sleep")
#endif

#include <boost/core/detail/sp_thread_yield.hpp>

namespace boost
{
namespace core
{

inline void sp_thread_sleep() BOOST_NOEXCEPT
{
    sp_thread_yield();
}

} // namespace core
} // namespace boost

#endif

#endif // #ifndef BOOST_CORE_DETAIL_SP_THREAD_SLEEP_HPP_INCLUDED
