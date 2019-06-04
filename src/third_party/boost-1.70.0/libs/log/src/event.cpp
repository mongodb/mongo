/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   event.cpp
 * \author Andrey Semashev
 * \date   24.07.2011
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>

#ifndef BOOST_LOG_NO_THREADS

#include <boost/assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/throw_exception.hpp>
#include <boost/log/detail/event.hpp>
#include <boost/log/exceptions.hpp>

#if defined(BOOST_LOG_EVENT_USE_FUTEX)

#include <stddef.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <boost/memory_order.hpp>

// Some Android NDKs (Google NDK and older Crystax.NET NDK versions) don't define SYS_futex
#if defined(SYS_futex)
#define BOOST_LOG_SYS_FUTEX SYS_futex
#else
#define BOOST_LOG_SYS_FUTEX __NR_futex
#endif

#if defined(FUTEX_WAIT_PRIVATE)
#define BOOST_LOG_FUTEX_WAIT FUTEX_WAIT_PRIVATE
#else
#define BOOST_LOG_FUTEX_WAIT FUTEX_WAIT
#endif

#if defined(FUTEX_WAKE_PRIVATE)
#define BOOST_LOG_FUTEX_WAKE FUTEX_WAKE_PRIVATE
#else
#define BOOST_LOG_FUTEX_WAKE FUTEX_WAKE
#endif

#elif defined(BOOST_LOG_EVENT_USE_POSIX_SEMAPHORE)

#include <errno.h>
#include <semaphore.h>
#include <boost/memory_order.hpp>
#include <boost/atomic/fences.hpp>

#elif defined(BOOST_LOG_EVENT_USE_WINAPI)

#include <windows.h>
#include <boost/detail/interlocked.hpp>

#else

#include <boost/thread/locks.hpp>

#endif

#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

#if defined(BOOST_LOG_EVENT_USE_FUTEX)

//! Default constructor
BOOST_LOG_API futex_based_event::futex_based_event() : m_state(0)
{
}

//! Destructor
BOOST_LOG_API futex_based_event::~futex_based_event()
{
}

//! Waits for the object to become signalled
BOOST_LOG_API void futex_based_event::wait()
{
    if (m_state.exchange(0, boost::memory_order_acq_rel) == 0)
    {
        while (true)
        {
            if (::syscall(BOOST_LOG_SYS_FUTEX, &m_state.storage(), BOOST_LOG_FUTEX_WAIT, 0, NULL, NULL, 0) == 0)
            {
                // Another thread has set the event while sleeping
                break;
            }

            const int err = errno;
            if (err == EWOULDBLOCK)
            {
                // Another thread has set the event before sleeping
                break;
            }
            else if (BOOST_UNLIKELY(err != EINTR))
            {
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to block on the futex", (err));
            }
        }

        m_state.store(0, boost::memory_order_relaxed);
    }
}

//! Sets the object to a signalled state
BOOST_LOG_API void futex_based_event::set_signalled()
{
    if (m_state.exchange(1, boost::memory_order_release) == 0)
    {
        if (BOOST_UNLIKELY(::syscall(BOOST_LOG_SYS_FUTEX, &m_state.storage(), BOOST_LOG_FUTEX_WAKE, 1, NULL, NULL, 0) < 0))
        {
            const int err = errno;
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to wake threads blocked on the futex", (err));
        }
    }
}

#elif defined(BOOST_LOG_EVENT_USE_POSIX_SEMAPHORE)

//! Default constructor
BOOST_LOG_API sem_based_event::sem_based_event() : m_state()
{
    if (BOOST_UNLIKELY(sem_init(&m_semaphore, 0, 0) != 0))
    {
        const int err = errno;
        BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to initialize semaphore", (err));
    }
}

//! Destructor
BOOST_LOG_API sem_based_event::~sem_based_event()
{
    BOOST_VERIFY(sem_destroy(&m_semaphore) == 0);
}

//! Waits for the object to become signalled
BOOST_LOG_API void sem_based_event::wait()
{
    boost::atomic_thread_fence(boost::memory_order_acq_rel);
    while (true)
    {
        if (sem_wait(&m_semaphore) != 0)
        {
            const int err = errno;
            if (BOOST_UNLIKELY(err != EINTR))
            {
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to block on the semaphore", (err));
            }
        }
        else
            break;
    }
    m_state.clear(boost::memory_order_relaxed);
}

//! Sets the object to a signalled state
BOOST_LOG_API void sem_based_event::set_signalled()
{
    if (!m_state.test_and_set(boost::memory_order_release))
    {
        if (BOOST_UNLIKELY(sem_post(&m_semaphore) != 0))
        {
            const int err = errno;
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to wake the blocked thread", (err));
        }
    }
}

#elif defined(BOOST_LOG_EVENT_USE_WINAPI)

//! Default constructor
BOOST_LOG_API winapi_based_event::winapi_based_event() :
    m_state(0),
    m_event(CreateEventA(NULL, false, false, NULL))
{
    if (BOOST_UNLIKELY(!m_event))
    {
        const DWORD err = GetLastError();
        BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to create Windows event", (err));
    }
}

//! Destructor
BOOST_LOG_API winapi_based_event::~winapi_based_event()
{
    BOOST_VERIFY(CloseHandle(m_event) != 0);
}

//! Waits for the object to become signalled
BOOST_LOG_API void winapi_based_event::wait()
{
    // On Windows we assume that memory view is always actual (Intel x86 and x86_64 arch)
    if (const_cast< volatile boost::uint32_t& >(m_state) == 0)
    {
        if (BOOST_UNLIKELY(WaitForSingleObject(m_event, INFINITE) != 0))
        {
            const DWORD err = GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to block on Windows event", (err));
        }
    }
    const_cast< volatile boost::uint32_t& >(m_state) = 0;
}

//! Sets the object to a signalled state
BOOST_LOG_API void winapi_based_event::set_signalled()
{
    if (BOOST_INTERLOCKED_COMPARE_EXCHANGE(reinterpret_cast< long* >(&m_state), 1, 0) == 0)
    {
        if (BOOST_UNLIKELY(SetEvent(m_event) == 0))
        {
            const DWORD err = GetLastError();
            const_cast< volatile boost::uint32_t& >(m_state) = 0;
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to wake the blocked thread", (err));
        }
    }
}

#else

//! Default constructor
BOOST_LOG_API generic_event::generic_event() : m_state(false)
{
}

//! Destructor
BOOST_LOG_API generic_event::~generic_event()
{
}

//! Waits for the object to become signalled
BOOST_LOG_API void generic_event::wait()
{
    boost::unique_lock< boost::mutex > lock(m_mutex);
    while (!m_state)
    {
        m_cond.wait(lock);
    }
    m_state = false;
}

//! Sets the object to a signalled state
BOOST_LOG_API void generic_event::set_signalled()
{
    boost::lock_guard< boost::mutex > lock(m_mutex);
    if (!m_state)
    {
        m_state = true;
        m_cond.notify_one();
    }
}

#endif

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_NO_THREADS
