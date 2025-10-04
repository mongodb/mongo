/*
 *          Copyright Andrey Semashev 2007 - 2021.
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

#if defined(BOOST_LOG_EVENT_USE_ATOMIC)

#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/atomic/fences.hpp>

#elif defined(BOOST_LOG_EVENT_USE_POSIX_SEMAPHORE)

#include <errno.h>
#include <semaphore.h>
#include <boost/memory_order.hpp>
#include <boost/atomic/fences.hpp>

#elif defined(BOOST_LOG_EVENT_USE_WINAPI)

#include <windows.h>
#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/atomic/fences.hpp>

#else

#include <mutex>

#endif

#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

#if defined(BOOST_LOG_EVENT_USE_ATOMIC)

//! Waits for the object to become signalled
BOOST_LOG_API void atomic_based_event::wait()
{
    while (m_state.exchange(0u, boost::memory_order_acq_rel) == 0u)
    {
        m_state.wait(0u, boost::memory_order_relaxed);
    }
}

//! Sets the object to a signalled state
BOOST_LOG_API void atomic_based_event::set_signalled()
{
    if (m_state.load(boost::memory_order_relaxed) != 0u)
    {
        boost::atomic_thread_fence(boost::memory_order_release);
    }
    else if (m_state.exchange(1u, boost::memory_order_release) == 0u)
    {
        m_state.notify_one();
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
    m_state(0u),
    m_event(NULL)
{
    if (!m_state.has_native_wait_notify())
    {
        m_event = CreateEventA(NULL, false, false, NULL);
        if (BOOST_UNLIKELY(!m_event))
        {
            const DWORD err = GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to create Windows event", (err));
        }
    }
}

//! Destructor
BOOST_LOG_API winapi_based_event::~winapi_based_event()
{
    if (!!m_event)
    {
        BOOST_VERIFY(CloseHandle(m_event) != 0);
    }
}

//! Waits for the object to become signalled
BOOST_LOG_API void winapi_based_event::wait()
{
    if (!m_event)
    {
        while (m_state.exchange(0u, boost::memory_order_acq_rel) == 0u)
        {
            m_state.wait(0u, boost::memory_order_relaxed);
        }
    }
    else
    {
        while (m_state.exchange(0u, boost::memory_order_acq_rel) == 0u)
        {
            if (BOOST_UNLIKELY(WaitForSingleObject(m_event, INFINITE) != 0))
            {
                const DWORD err = GetLastError();
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to block on Windows event", (err));
            }
        }
    }
}

//! Sets the object to a signalled state
BOOST_LOG_API void winapi_based_event::set_signalled()
{
    if (m_state.load(boost::memory_order_relaxed) != 0u)
    {
        boost::atomic_thread_fence(boost::memory_order_release);
    }
    else if (m_state.exchange(1u, boost::memory_order_release) == 0u)
    {
        if (!m_event)
        {
            m_state.notify_one();
        }
        else
        {
            if (BOOST_UNLIKELY(SetEvent(m_event) == 0))
            {
                const DWORD err = GetLastError();
                m_state.store(0u, boost::memory_order_relaxed);
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to wake the blocked thread", (err));
            }
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
    std::unique_lock< std::mutex > lock(m_mutex);
    while (!m_state)
    {
        m_cond.wait(lock);
    }
    m_state = false;
}

//! Sets the object to a signalled state
BOOST_LOG_API void generic_event::set_signalled()
{
    std::lock_guard< std::mutex > lock(m_mutex);
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
