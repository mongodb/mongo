// Copyright (C) 2001-2003
// William E. Kempf
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_CONDITION_WEK070601_HPP
#define BOOST_CONDITION_WEK070601_HPP

#include <boost/thread/detail/config.hpp>

#include <boost/thread/exceptions.hpp>
#include <boost/utility.hpp>
#include <boost/thread/detail/lock.hpp>

#if defined(BOOST_HAS_PTHREADS)
#   include <pthread.h>
#elif defined(BOOST_HAS_MPTASKS)
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

namespace detail {

class BOOST_THREAD_DECL condition_impl : private noncopyable
{
    friend class condition;

public:
    condition_impl();
    ~condition_impl();

    void notify_one();
    void notify_all();

#if (defined(BOOST_HAS_WINTHREADS) || defined(BOOST_HAS_MPTASKS))
    void enter_wait();
    void do_wait();
    bool do_timed_wait(const xtime& xt);
#elif defined(BOOST_HAS_PTHREADS)
    void do_wait(pthread_mutex_t* pmutex);
    bool do_timed_wait(const xtime& xt, pthread_mutex_t* pmutex);
#endif

#if defined(BOOST_HAS_WINTHREADS)
    void* m_gate;
    void* m_queue;
    void* m_mutex;
    unsigned m_gone;  // # threads that timed out and never made it to m_queue
    unsigned long m_blocked; // # threads blocked on the condition
    unsigned m_waiting; // # threads no longer waiting for the condition but
                        // still waiting to be removed from m_queue
#elif defined(BOOST_HAS_PTHREADS)
    pthread_cond_t m_condition;
#elif defined(BOOST_HAS_MPTASKS)
    MPSemaphoreID m_gate;
    MPSemaphoreID m_queue;
    threads::mac::detail::scoped_critical_region m_mutex;
    threads::mac::detail::scoped_critical_region m_mutex_mutex;
    unsigned m_gone; // # threads that timed out and never made it to m_queue
    unsigned long m_blocked; // # threads blocked on the condition
    unsigned m_waiting; // # threads no longer waiting for the condition but
                        // still waiting to be removed from m_queue
#endif
};

} // namespace detail

class condition : private noncopyable
{
public:
    condition() { }
    ~condition() { }

    void notify_one() { m_impl.notify_one(); }
    void notify_all() { m_impl.notify_all(); }

    template <typename L>
    void wait(L& lock)
    {
        if (!lock)
            throw lock_error();

        do_wait(lock.m_mutex);
    }

    template <typename L, typename Pr>
    void wait(L& lock, Pr pred)
    {
        if (!lock)
            throw lock_error();

        while (!pred())
            do_wait(lock.m_mutex);
    }

    template <typename L>
    bool timed_wait(L& lock, const xtime& xt)
    {
        if (!lock)
            throw lock_error();

        return do_timed_wait(lock.m_mutex, xt);
    }

    template <typename L, typename Pr>
    bool timed_wait(L& lock, const xtime& xt, Pr pred)
    {
        if (!lock)
            throw lock_error();

        while (!pred())
        {
            if (!do_timed_wait(lock.m_mutex, xt))
                return false;
        }

        return true;
    }

private:
    detail::condition_impl m_impl;

    template <typename M>
    void do_wait(M& mutex)
    {
#if (defined(BOOST_HAS_WINTHREADS) || defined(BOOST_HAS_MPTASKS))
        m_impl.enter_wait();
#endif

        typedef detail::thread::lock_ops<M>
#if defined(__HP_aCC) && __HP_aCC <= 33900 && !defined(BOOST_STRICT_CONFIG)
# define lock_ops lock_ops_  // HP confuses lock_ops witht the template
#endif
            lock_ops;

        typename lock_ops::lock_state state;
        lock_ops::unlock(mutex, state);

#if defined(BOOST_HAS_PTHREADS)
        m_impl.do_wait(state.pmutex);
#elif (defined(BOOST_HAS_WINTHREADS) || defined(BOOST_HAS_MPTASKS))
        m_impl.do_wait();
#endif

        lock_ops::lock(mutex, state);
#undef lock_ops
    }

    template <typename M>
    bool do_timed_wait(M& mutex, const xtime& xt)
    {
#if (defined(BOOST_HAS_WINTHREADS) || defined(BOOST_HAS_MPTASKS))
        m_impl.enter_wait();
#endif

        typedef detail::thread::lock_ops<M>
#if defined(__HP_aCC) && __HP_aCC <= 33900 && !defined(BOOST_STRICT_CONFIG)
# define lock_ops lock_ops_  // HP confuses lock_ops witht the template
#endif
            lock_ops;

        typename lock_ops::lock_state state;
        lock_ops::unlock(mutex, state);

        bool ret = false;

#if defined(BOOST_HAS_PTHREADS)
        ret = m_impl.do_timed_wait(xt, state.pmutex);
#elif (defined(BOOST_HAS_WINTHREADS) || defined(BOOST_HAS_MPTASKS))
        ret = m_impl.do_timed_wait(xt);
#endif

        lock_ops::lock(mutex, state);
#undef lock_ops

        return ret;
    }
};
#ifdef BOOST_MSVC
#   pragma warning(pop)
#endif
} // namespace boost

// Change Log:
//    8 Feb 01  WEKEMPF Initial version.
//   22 May 01  WEKEMPF Modified to use xtime for time outs.
//   23 May 01  WEKEMPF Removed "duration" timed_waits, as they are too
//                      difficult to use with spurious wakeups.
//    3 Jan 03  WEKEMPF Modified for DLL implementation.

#endif // BOOST_CONDITION_WEK070601_HPP
