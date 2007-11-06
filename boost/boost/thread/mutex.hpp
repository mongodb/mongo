// Copyright (C) 2001-2003
// William E. Kempf
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MUTEX_WEK070601_HPP
#define BOOST_MUTEX_WEK070601_HPP

#include <boost/thread/detail/config.hpp>

#include <boost/utility.hpp>
#include <boost/thread/detail/lock.hpp>

#if defined(BOOST_HAS_PTHREADS)
#   include <pthread.h>
#endif

#if defined(BOOST_HAS_MPTASKS)
#   include "scoped_critical_region.hpp"
#endif

namespace boost {

struct xtime;
// disable warnings about non dll import
// see: http://www.boost.org/more/separate_compilation.html#dlls
#ifdef BOOST_MSVC
#   pragma warning(push)
#   pragma warning(disable: 4251 4231 4660 4275)
#endif

class BOOST_THREAD_DECL mutex
    : private noncopyable
{
public:
    friend class detail::thread::lock_ops<mutex>;

    typedef detail::thread::scoped_lock<mutex> scoped_lock;

    mutex();
    ~mutex();

private:
#if defined(BOOST_HAS_WINTHREADS)
    typedef void* cv_state;
#elif defined(BOOST_HAS_PTHREADS)
    struct cv_state
    {
        pthread_mutex_t* pmutex;
    };
#elif defined(BOOST_HAS_MPTASKS)
    struct cv_state
    {
    };
#endif
    void do_lock();
    void do_unlock();
    void do_lock(cv_state& state);
    void do_unlock(cv_state& state);

#if defined(BOOST_HAS_WINTHREADS)
    void* m_mutex;
    bool m_critical_section;
#elif defined(BOOST_HAS_PTHREADS)
    pthread_mutex_t m_mutex;
#elif defined(BOOST_HAS_MPTASKS)
    threads::mac::detail::scoped_critical_region m_mutex;
    threads::mac::detail::scoped_critical_region m_mutex_mutex;
#endif
};

class BOOST_THREAD_DECL try_mutex
    : private noncopyable
{
public:
    friend class detail::thread::lock_ops<try_mutex>;

    typedef detail::thread::scoped_lock<try_mutex> scoped_lock;
    typedef detail::thread::scoped_try_lock<try_mutex> scoped_try_lock;

    try_mutex();
    ~try_mutex();

private:
#if defined(BOOST_HAS_WINTHREADS)
    typedef void* cv_state;
#elif defined(BOOST_HAS_PTHREADS)
    struct cv_state
    {
        pthread_mutex_t* pmutex;
    };
#elif defined(BOOST_HAS_MPTASKS)
    struct cv_state
    {
    };
#endif
    void do_lock();
    bool do_trylock();
    void do_unlock();
    void do_lock(cv_state& state);
    void do_unlock(cv_state& state);

#if defined(BOOST_HAS_WINTHREADS)
    void* m_mutex;
    bool m_critical_section;
#elif defined(BOOST_HAS_PTHREADS)
    pthread_mutex_t m_mutex;
#elif defined(BOOST_HAS_MPTASKS)
    threads::mac::detail::scoped_critical_region m_mutex;
    threads::mac::detail::scoped_critical_region m_mutex_mutex;
#endif
};

class BOOST_THREAD_DECL timed_mutex
    : private noncopyable
{
public:
    friend class detail::thread::lock_ops<timed_mutex>;

    typedef detail::thread::scoped_lock<timed_mutex> scoped_lock;
    typedef detail::thread::scoped_try_lock<timed_mutex> scoped_try_lock;
    typedef detail::thread::scoped_timed_lock<timed_mutex> scoped_timed_lock;

    timed_mutex();
    ~timed_mutex();

private:
#if defined(BOOST_HAS_WINTHREADS)
    typedef void* cv_state;
#elif defined(BOOST_HAS_PTHREADS)
    struct cv_state
    {
        pthread_mutex_t* pmutex;
    };
#elif defined(BOOST_HAS_MPTASKS)
    struct cv_state
    {
    };
#endif
    void do_lock();
    bool do_trylock();
    bool do_timedlock(const xtime& xt);
    void do_unlock();
    void do_lock(cv_state& state);
    void do_unlock(cv_state& state);

#if defined(BOOST_HAS_WINTHREADS)
    void* m_mutex;
#elif defined(BOOST_HAS_PTHREADS)
    pthread_mutex_t m_mutex;
    pthread_cond_t m_condition;
    bool m_locked;
#elif defined(BOOST_HAS_MPTASKS)
    threads::mac::detail::scoped_critical_region m_mutex;
    threads::mac::detail::scoped_critical_region m_mutex_mutex;
#endif
};
#ifdef BOOST_MSVC
#   pragma warning(pop)
#endif
} // namespace boost

// Change Log:
//    8 Feb 01  WEKEMPF Initial version.
//   22 May 01  WEKEMPF Modified to use xtime for time outs.  Factored out
//                      to three classes, mutex, try_mutex and timed_mutex.
//    3 Jan 03  WEKEMPF Modified for DLL implementation.

#endif // BOOST_MUTEX_WEK070601_HPP
