/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   posix/ipc_sync_wrappers.hpp
 * \author Andrey Semashev
 * \date   05.01.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_POSIX_IPC_SYNC_WRAPPERS_HPP_INCLUDED_
#define BOOST_LOG_POSIX_IPC_SYNC_WRAPPERS_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <pthread.h>
#include <cerrno>
#include <cstddef>
#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>
// Use Boost.Interprocess to detect if process-shared pthread primitives are supported
#include <boost/interprocess/detail/workaround.hpp>
#if !defined(BOOST_INTERPROCESS_POSIX_PROCESS_SHARED)
#include <boost/core/explicit_operator_bool.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#undef BOOST_LOG_HAS_PTHREAD_MUTEX_ROBUST
#endif
#include <boost/log/exceptions.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace ipc {

namespace aux {

#if defined(BOOST_INTERPROCESS_POSIX_PROCESS_SHARED)

#if defined(BOOST_LOG_HAS_PTHREAD_MUTEX_ROBUST)
struct BOOST_SYMBOL_VISIBLE lock_owner_dead {};
#endif

//! Pthread mutex attributes
struct pthread_mutex_attributes
{
    pthread_mutexattr_t attrs;

    pthread_mutex_attributes()
    {
        int err = pthread_mutexattr_init(&this->attrs);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to initialize pthread mutex attributes", (err));
    }

    ~pthread_mutex_attributes()
    {
        BOOST_VERIFY(pthread_mutexattr_destroy(&this->attrs) == 0);
    }

    BOOST_DELETED_FUNCTION(pthread_mutex_attributes(pthread_mutex_attributes const&))
    BOOST_DELETED_FUNCTION(pthread_mutex_attributes& operator=(pthread_mutex_attributes const&))
};

//! Pthread condifion variable attributes
struct pthread_condition_variable_attributes
{
    pthread_condattr_t attrs;

    pthread_condition_variable_attributes()
    {
        int err = pthread_condattr_init(&this->attrs);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to initialize pthread condition variable attributes", (err));
    }

    ~pthread_condition_variable_attributes()
    {
        BOOST_VERIFY(pthread_condattr_destroy(&this->attrs) == 0);
    }

    BOOST_DELETED_FUNCTION(pthread_condition_variable_attributes(pthread_condition_variable_attributes const&))
    BOOST_DELETED_FUNCTION(pthread_condition_variable_attributes& operator=(pthread_condition_variable_attributes const&))
};

//! Interprocess mutex wrapper
struct interprocess_mutex
{
    struct auto_unlock
    {
        explicit auto_unlock(interprocess_mutex& mutex) BOOST_NOEXCEPT : m_mutex(mutex) {}
        ~auto_unlock() { m_mutex.unlock(); }

        BOOST_DELETED_FUNCTION(auto_unlock(auto_unlock const&))
        BOOST_DELETED_FUNCTION(auto_unlock& operator=(auto_unlock const&))

    private:
        interprocess_mutex& m_mutex;
    };

    pthread_mutex_t mutex;

    interprocess_mutex()
    {
        pthread_mutex_attributes attrs;
        int err = pthread_mutexattr_settype(&attrs.attrs, PTHREAD_MUTEX_NORMAL);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to set pthread mutex type", (err));
        err = pthread_mutexattr_setpshared(&attrs.attrs, PTHREAD_PROCESS_SHARED);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to make pthread mutex process-shared", (err));
#if defined(BOOST_LOG_HAS_PTHREAD_MUTEX_ROBUST)
        err = pthread_mutexattr_setrobust(&attrs.attrs, PTHREAD_MUTEX_ROBUST);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to make pthread mutex robust", (err));
#endif

        err = pthread_mutex_init(&this->mutex, &attrs.attrs);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to initialize pthread mutex", (err));
    }

    ~interprocess_mutex()
    {
        BOOST_VERIFY(pthread_mutex_destroy(&this->mutex) == 0);
    }

    void lock()
    {
        int err = pthread_mutex_lock(&this->mutex);
#if defined(BOOST_LOG_HAS_PTHREAD_MUTEX_ROBUST)
        if (BOOST_UNLIKELY(err == EOWNERDEAD))
            throw lock_owner_dead();
#endif
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to lock pthread mutex", (err));
    }

    void unlock() BOOST_NOEXCEPT
    {
        BOOST_VERIFY(pthread_mutex_unlock(&this->mutex) == 0);
    }

#if defined(BOOST_LOG_HAS_PTHREAD_MUTEX_ROBUST)
    void recover()
    {
        int err = pthread_mutex_consistent(&this->mutex);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to recover pthread mutex from a crashed thread", (err));
    }
#endif

    BOOST_DELETED_FUNCTION(interprocess_mutex(interprocess_mutex const&))
    BOOST_DELETED_FUNCTION(interprocess_mutex& operator=(interprocess_mutex const&))
};

//! Interprocess condition variable wrapper
struct interprocess_condition_variable
{
    pthread_cond_t cond;

    interprocess_condition_variable()
    {
        pthread_condition_variable_attributes attrs;
        int err = pthread_condattr_setpshared(&attrs.attrs, PTHREAD_PROCESS_SHARED);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to make pthread condition variable process-shared", (err));

        err = pthread_cond_init(&this->cond, &attrs.attrs);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to initialize pthread condition variable", (err));
    }

    ~interprocess_condition_variable()
    {
        BOOST_VERIFY(pthread_cond_destroy(&this->cond) == 0);
    }

    void notify_one()
    {
        int err = pthread_cond_signal(&this->cond);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to notify one thread on a pthread condition variable", (err));
    }

    void notify_all()
    {
        int err = pthread_cond_broadcast(&this->cond);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to notify all threads on a pthread condition variable", (err));
    }

    void wait(interprocess_mutex& mutex)
    {
        int err = pthread_cond_wait(&this->cond, &mutex.mutex);
        if (BOOST_UNLIKELY(err != 0))
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to wait on a pthread condition variable", (err));
    }

    BOOST_DELETED_FUNCTION(interprocess_condition_variable(interprocess_condition_variable const&))
    BOOST_DELETED_FUNCTION(interprocess_condition_variable& operator=(interprocess_condition_variable const&))
};

#else // defined(BOOST_INTERPROCESS_POSIX_PROCESS_SHARED)

// If there are no process-shared pthread primitives, use whatever emulation Boost.Interprocess implements
struct interprocess_mutex
{
    struct auto_unlock
    {
        explicit auto_unlock(interprocess_mutex& mutex) BOOST_NOEXCEPT : m_mutex(mutex) {}
        ~auto_unlock() { m_mutex.unlock(); }

        BOOST_DELETED_FUNCTION(auto_unlock(auto_unlock const&))
        BOOST_DELETED_FUNCTION(auto_unlock& operator=(auto_unlock const&))

    private:
        interprocess_mutex& m_mutex;
    };

    BOOST_DEFAULTED_FUNCTION(interprocess_mutex(), {})

    // Members to emulate a lock interface
    typedef boost::interprocess::interprocess_mutex mutex_type;

    BOOST_EXPLICIT_OPERATOR_BOOL_NOEXCEPT()
    bool operator! () const BOOST_NOEXCEPT { return false; }
    mutex_type* mutex() BOOST_NOEXCEPT { return &m_mutex; }

    void lock()
    {
        m_mutex.lock();
    }

    void unlock() BOOST_NOEXCEPT
    {
        m_mutex.unlock();
    }

    mutex_type m_mutex;

    BOOST_DELETED_FUNCTION(interprocess_mutex(interprocess_mutex const&))
    BOOST_DELETED_FUNCTION(interprocess_mutex& operator=(interprocess_mutex const&))
};


typedef boost::interprocess::interprocess_condition interprocess_condition_variable;

#endif // defined(BOOST_INTERPROCESS_POSIX_PROCESS_SHARED)

} // namespace aux

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_POSIX_IPC_SYNC_WRAPPERS_HPP_INCLUDED_
