#ifndef BOOST_THREAD_PTHREAD_RECURSIVE_MUTEX_HPP
#define BOOST_THREAD_PTHREAD_RECURSIVE_MUTEX_HPP
// (C) Copyright 2007-8 Anthony Williams
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <pthread.h>
#include <boost/utility.hpp>
#include <boost/throw_exception.hpp>
#include <boost/thread/exceptions.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/assert.hpp>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <boost/date_time/posix_time/conversion.hpp>
#include <errno.h>
#include <boost/thread/pthread/timespec.hpp>
#include <boost/thread/pthread/pthread_mutex_scoped_lock.hpp>

#ifdef _POSIX_TIMEOUTS
#if _POSIX_TIMEOUTS >= 0
#define BOOST_PTHREAD_HAS_TIMEDLOCK
#endif
#endif

#if defined(BOOST_HAS_PTHREAD_MUTEXATTR_SETTYPE) && defined(BOOST_PTHREAD_HAS_TIMEDLOCK)
#define BOOST_USE_PTHREAD_RECURSIVE_TIMEDLOCK
#endif

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    class recursive_mutex
    {
    private:
        recursive_mutex(recursive_mutex const&);
        recursive_mutex& operator=(recursive_mutex const&);
        pthread_mutex_t m;
#ifndef BOOST_HAS_PTHREAD_MUTEXATTR_SETTYPE
        pthread_cond_t cond;
        bool is_locked;
        pthread_t owner;
        unsigned count;
#endif
    public:
        recursive_mutex()
        {
#ifdef BOOST_HAS_PTHREAD_MUTEXATTR_SETTYPE
            pthread_mutexattr_t attr;

            int const init_attr_res=pthread_mutexattr_init(&attr);
            if(init_attr_res)
            {
                boost::throw_exception(thread_resource_error());
            }
            int const set_attr_res=pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
            if(set_attr_res)
            {
                BOOST_VERIFY(!pthread_mutexattr_destroy(&attr));
                boost::throw_exception(thread_resource_error());
            }

            int const res=pthread_mutex_init(&m,&attr);
            if(res)
            {
                BOOST_VERIFY(!pthread_mutexattr_destroy(&attr));
                boost::throw_exception(thread_resource_error());
            }
            BOOST_VERIFY(!pthread_mutexattr_destroy(&attr));
#else
            int const res=pthread_mutex_init(&m,NULL);
            if(res)
            {
                boost::throw_exception(thread_resource_error());
            }
            int const res2=pthread_cond_init(&cond,NULL);
            if(res2)
            {
                BOOST_VERIFY(!pthread_mutex_destroy(&m));
                boost::throw_exception(thread_resource_error());
            }
            is_locked=false;
            count=0;
#endif
        }
        ~recursive_mutex()
        {
            BOOST_VERIFY(!pthread_mutex_destroy(&m));
#ifndef BOOST_HAS_PTHREAD_MUTEXATTR_SETTYPE
            BOOST_VERIFY(!pthread_cond_destroy(&cond));
#endif
        }

#ifdef BOOST_HAS_PTHREAD_MUTEXATTR_SETTYPE
        void lock()
        {
            BOOST_VERIFY(!pthread_mutex_lock(&m));
        }

        void unlock()
        {
            BOOST_VERIFY(!pthread_mutex_unlock(&m));
        }

        bool try_lock()
        {
            int const res=pthread_mutex_trylock(&m);
            BOOST_ASSERT(!res || res==EBUSY);
            return !res;
        }
        typedef pthread_mutex_t* native_handle_type;
        native_handle_type native_handle()
        {
            return &m;
        }

#else
        void lock()
        {
            boost::pthread::pthread_mutex_scoped_lock const local_lock(&m);
            if(is_locked && pthread_equal(owner,pthread_self()))
            {
                ++count;
                return;
            }

            while(is_locked)
            {
                BOOST_VERIFY(!pthread_cond_wait(&cond,&m));
            }
            is_locked=true;
            ++count;
            owner=pthread_self();
        }

        void unlock()
        {
            boost::pthread::pthread_mutex_scoped_lock const local_lock(&m);
            if(!--count)
            {
                is_locked=false;
            }
            BOOST_VERIFY(!pthread_cond_signal(&cond));
        }

        bool try_lock()
        {
            boost::pthread::pthread_mutex_scoped_lock const local_lock(&m);
            if(is_locked && !pthread_equal(owner,pthread_self()))
            {
                return false;
            }
            is_locked=true;
            ++count;
            owner=pthread_self();
            return true;
        }

#endif

        typedef unique_lock<recursive_mutex> scoped_lock;
        typedef detail::try_lock_wrapper<recursive_mutex> scoped_try_lock;
    };

    typedef recursive_mutex recursive_try_mutex;

    class recursive_timed_mutex
    {
    private:
        recursive_timed_mutex(recursive_timed_mutex const&);
        recursive_timed_mutex& operator=(recursive_timed_mutex const&);
    private:
        pthread_mutex_t m;
#ifndef BOOST_USE_PTHREAD_RECURSIVE_TIMEDLOCK
        pthread_cond_t cond;
        bool is_locked;
        pthread_t owner;
        unsigned count;
#endif
    public:
        recursive_timed_mutex()
        {
#ifdef BOOST_USE_PTHREAD_RECURSIVE_TIMEDLOCK
            pthread_mutexattr_t attr;

            int const init_attr_res=pthread_mutexattr_init(&attr);
            if(init_attr_res)
            {
                boost::throw_exception(thread_resource_error());
            }
            int const set_attr_res=pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
            if(set_attr_res)
            {
                boost::throw_exception(thread_resource_error());
            }

            int const res=pthread_mutex_init(&m,&attr);
            if(res)
            {
                BOOST_VERIFY(!pthread_mutexattr_destroy(&attr));
                boost::throw_exception(thread_resource_error());
            }
            BOOST_VERIFY(!pthread_mutexattr_destroy(&attr));
#else
            int const res=pthread_mutex_init(&m,NULL);
            if(res)
            {
                boost::throw_exception(thread_resource_error());
            }
            int const res2=pthread_cond_init(&cond,NULL);
            if(res2)
            {
                BOOST_VERIFY(!pthread_mutex_destroy(&m));
                boost::throw_exception(thread_resource_error());
            }
            is_locked=false;
            count=0;
#endif
        }
        ~recursive_timed_mutex()
        {
            BOOST_VERIFY(!pthread_mutex_destroy(&m));
#ifndef BOOST_USE_PTHREAD_RECURSIVE_TIMEDLOCK
            BOOST_VERIFY(!pthread_cond_destroy(&cond));
#endif
        }

        template<typename TimeDuration>
        bool timed_lock(TimeDuration const & relative_time)
        {
            return timed_lock(get_system_time()+relative_time);
        }

#ifdef BOOST_USE_PTHREAD_RECURSIVE_TIMEDLOCK
        void lock()
        {
            BOOST_VERIFY(!pthread_mutex_lock(&m));
        }

        void unlock()
        {
            BOOST_VERIFY(!pthread_mutex_unlock(&m));
        }

        bool try_lock()
        {
            int const res=pthread_mutex_trylock(&m);
            BOOST_ASSERT(!res || res==EBUSY);
            return !res;
        }
        bool timed_lock(system_time const & abs_time)
        {
            struct timespec const timeout=detail::get_timespec(abs_time);
            int const res=pthread_mutex_timedlock(&m,&timeout);
            BOOST_ASSERT(!res || res==ETIMEDOUT);
            return !res;
        }

        typedef pthread_mutex_t* native_handle_type;
        native_handle_type native_handle()
        {
            return &m;
        }

#else
        void lock()
        {
            boost::pthread::pthread_mutex_scoped_lock const local_lock(&m);
            if(is_locked && pthread_equal(owner,pthread_self()))
            {
                ++count;
                return;
            }

            while(is_locked)
            {
                BOOST_VERIFY(!pthread_cond_wait(&cond,&m));
            }
            is_locked=true;
            ++count;
            owner=pthread_self();
        }

        void unlock()
        {
            boost::pthread::pthread_mutex_scoped_lock const local_lock(&m);
            if(!--count)
            {
                is_locked=false;
            }
            BOOST_VERIFY(!pthread_cond_signal(&cond));
        }

        bool try_lock()
        {
            boost::pthread::pthread_mutex_scoped_lock const local_lock(&m);
            if(is_locked && !pthread_equal(owner,pthread_self()))
            {
                return false;
            }
            is_locked=true;
            ++count;
            owner=pthread_self();
            return true;
        }

        bool timed_lock(system_time const & abs_time)
        {
            struct timespec const timeout=detail::get_timespec(abs_time);
            boost::pthread::pthread_mutex_scoped_lock const local_lock(&m);
            if(is_locked && pthread_equal(owner,pthread_self()))
            {
                ++count;
                return true;
            }
            while(is_locked)
            {
                int const cond_res=pthread_cond_timedwait(&cond,&m,&timeout);
                if(cond_res==ETIMEDOUT)
                {
                    return false;
                }
                BOOST_ASSERT(!cond_res);
            }
            is_locked=true;
            ++count;
            owner=pthread_self();
            return true;
        }
#endif

        typedef unique_lock<recursive_timed_mutex> scoped_timed_lock;
        typedef detail::try_lock_wrapper<recursive_timed_mutex> scoped_try_lock;
        typedef scoped_timed_lock scoped_lock;
    };

}

#include <boost/config/abi_suffix.hpp>

#endif
