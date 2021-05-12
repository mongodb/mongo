/*
 *          Copyright Andrey Semashev 2007 - 2018.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   timestamp.cpp
 * \author Andrey Semashev
 * \date   31.07.2011
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <boost/log/detail/timestamp.hpp>

#if defined(BOOST_WINDOWS) && !defined(__CYGWIN__)
#include <cstddef>
#include <cstdlib>
#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/winapi/dll.hpp>
#include <boost/winapi/time.hpp>
#include <boost/winapi/event.hpp>
#include <boost/winapi/handles.hpp>
#include <boost/winapi/thread_pool.hpp>
#else
#include <unistd.h> // for config macros
#if defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)
#include <mach/mach_time.h>
#include <mach/kern_return.h>
#include <boost/log/utility/once_block.hpp>
#include <boost/system/error_code.hpp>
#endif
#include <time.h>
#include <errno.h>
#include <boost/throw_exception.hpp>
#include <boost/log/exceptions.hpp>
#endif
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

#if defined(BOOST_WINDOWS) && !defined(__CYGWIN__)

#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

// Directly use API from Vista and later
BOOST_LOG_API get_tick_count_t get_tick_count = &boost::winapi::GetTickCount64;

#else // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

BOOST_LOG_ANONYMOUS_NAMESPACE {

enum init_state
{
    uninitialized = 0,
    in_progress,
    initialized
};

struct get_tick_count64_state
{
    boost::atomic< uint64_t > ticks;
    boost::atomic< init_state > init;
    boost::winapi::HANDLE_ wait_event;
    boost::winapi::HANDLE_ wait_handle;
};

// Zero-initialized initially
BOOST_ALIGNMENT(BOOST_LOG_CPU_CACHE_LINE_SIZE) static get_tick_count64_state g_state;

//! Artifical implementation of GetTickCount64
uint64_t BOOST_WINAPI_WINAPI_CC get_tick_count64()
{
    // Note: Even in single-threaded builds we have to implement get_tick_count64 in a thread-safe way because
    //       it can be called in the system thread pool during refreshes concurrently with user's calls.
    uint64_t old_state = g_state.ticks.load(boost::memory_order_acquire);

    uint32_t new_ticks = boost::winapi::GetTickCount();

    uint32_t old_ticks = static_cast< uint32_t >(old_state & UINT64_C(0x00000000ffffffff));
    uint64_t new_state = ((old_state & UINT64_C(0xffffffff00000000)) + (static_cast< uint64_t >(new_ticks < old_ticks) << 32)) | static_cast< uint64_t >(new_ticks);

    g_state.ticks.store(new_state, boost::memory_order_release);

    return new_state;
}

//! The function is called periodically in the system thread pool to make sure g_state.ticks is timely updated
void BOOST_WINAPI_NTAPI_CC refresh_get_tick_count64(boost::winapi::PVOID_, boost::winapi::BOOLEAN_)
{
    get_tick_count64();
}

//! Cleanup function to stop get_tick_count64 refreshes
void cleanup_get_tick_count64()
{
    if (g_state.wait_handle)
    {
        boost::winapi::UnregisterWait(g_state.wait_handle);
        g_state.wait_handle = NULL;
    }

    if (g_state.wait_event)
    {
        boost::winapi::CloseHandle(g_state.wait_event);
        g_state.wait_event = NULL;
    }
}

uint64_t BOOST_WINAPI_WINAPI_CC get_tick_count_init()
{
    boost::winapi::HMODULE_ hKernel32 = boost::winapi::GetModuleHandleW(L"kernel32.dll");
    if (hKernel32)
    {
        get_tick_count_t p = (get_tick_count_t)boost::winapi::get_proc_address(hKernel32, "GetTickCount64");
        if (p)
        {
            // Use native API
            const_cast< get_tick_count_t volatile& >(get_tick_count) = p;
            return p();
        }
    }

    // No native API available. Use emulation with periodic refreshes to make sure the GetTickCount wrap arounds are properly counted.
    init_state old_init = uninitialized;
    if (g_state.init.compare_exchange_strong(old_init, in_progress, boost::memory_order_acq_rel, boost::memory_order_relaxed))
    {
        if (!g_state.wait_event)
            g_state.wait_event = boost::winapi::create_anonymous_event(NULL, false, false);
        if (g_state.wait_event)
        {
            boost::winapi::BOOL_ res = boost::winapi::RegisterWaitForSingleObject(&g_state.wait_handle, g_state.wait_event, &refresh_get_tick_count64, NULL, 0x7fffffff, boost::winapi::WT_EXECUTEINWAITTHREAD_);
            if (res)
            {
                std::atexit(&cleanup_get_tick_count64);

                const_cast< get_tick_count_t volatile& >(get_tick_count) = &get_tick_count64;
                g_state.init.store(initialized, boost::memory_order_release);
                goto finish;
            }
        }

        g_state.init.store(uninitialized, boost::memory_order_release);
    }

finish:
    return get_tick_count64();
}

} // namespace

BOOST_LOG_API get_tick_count_t get_tick_count = &get_tick_count_init;

#endif // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

#elif (defined(_POSIX_TIMERS) && (_POSIX_TIMERS+0) > 0)  /* POSIX timers supported */ \
      || defined(__GNU__) || defined(__OpenBSD__) || defined(__CloudABI__)  /* GNU Hurd, OpenBSD and Nuxi CloudABI don't support POSIX timers fully but do provide clock_gettime() */

BOOST_LOG_API int64_t duration::milliseconds() const
{
    // Timestamps are always in nanoseconds
    return m_ticks / INT64_C(1000000);
}

BOOST_LOG_ANONYMOUS_NAMESPACE {

/*!
 * \c get_timestamp implementation based on POSIX realtime clock.
 * Note that this implementation is only used as a last resort since
 * this timer can be manually set and may jump due to DST change.
 */
timestamp get_timestamp_realtime_clock()
{
    timespec ts;
    if (BOOST_UNLIKELY(clock_gettime(CLOCK_REALTIME, &ts) != 0))
    {
        const int err = errno;
        BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to acquire current time", (err));
    }

    return timestamp(static_cast< uint64_t >(ts.tv_sec) * UINT64_C(1000000000) + ts.tv_nsec);
}

#   if defined(_POSIX_MONOTONIC_CLOCK)

//! \c get_timestamp implementation based on POSIX monotonic clock
timestamp get_timestamp_monotonic_clock()
{
    timespec ts;
    if (BOOST_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0))
    {
        const int err = errno;
        if (err == EINVAL)
        {
            // The current platform does not support monotonic timer.
            // Fall back to realtime clock, which is not exactly what we need
            // but is better than nothing.
            get_timestamp = &get_timestamp_realtime_clock;
            return get_timestamp_realtime_clock();
        }
        BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to acquire current time", (err));
    }

    return timestamp(static_cast< uint64_t >(ts.tv_sec) * UINT64_C(1000000000) + ts.tv_nsec);
}

#       define BOOST_LOG_DEFAULT_GET_TIMESTAMP get_timestamp_monotonic_clock

#   else // if defined(_POSIX_MONOTONIC_CLOCK)
#       define BOOST_LOG_DEFAULT_GET_TIMESTAMP get_timestamp_realtime_clock
#   endif // if defined(_POSIX_MONOTONIC_CLOCK)

} // namespace

// Use POSIX API
BOOST_LOG_API get_timestamp_t get_timestamp = &BOOST_LOG_DEFAULT_GET_TIMESTAMP;

#   undef BOOST_LOG_DEFAULT_GET_TIMESTAMP

#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)

BOOST_LOG_API int64_t duration::milliseconds() const
{
    static mach_timebase_info_data_t timebase_info = {};
    BOOST_LOG_ONCE_BLOCK()
    {
        kern_return_t err = mach_timebase_info(&timebase_info);
        if (err != KERN_SUCCESS)
        {
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to initialize timebase info", (boost::system::errc::not_supported));
        }
    }

    // Often the timebase rational equals 1, we can optimize for this case
    if (timebase_info.numer == timebase_info.denom)
    {
        // Timestamps are in nanoseconds
        return m_ticks / INT64_C(1000000);
    }
    else
    {
        return (m_ticks * timebase_info.numer) / (INT64_C(1000000) * timebase_info.denom);
    }
}

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! \c get_timestamp implementation based on MacOS X absolute time
timestamp get_timestamp_mach()
{
    return timestamp(mach_absolute_time());
}

} // namespace

// Use MacOS X API
BOOST_LOG_API get_timestamp_t get_timestamp = &get_timestamp_mach;

#else

#   error Boost.Log: Timestamp generation is not supported for your platform

#endif

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
