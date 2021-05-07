#ifndef BOOST_PTHREAD_MUTEX_SCOPED_LOCK_HPP
#define BOOST_PTHREAD_MUTEX_SCOPED_LOCK_HPP
//  (C) Copyright 2007-8 Anthony Williams
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <pthread.h>
#include <boost/assert.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
namespace posix {
#ifdef BOOST_THREAD_HAS_EINTR_BUG
  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_mutex_destroy(pthread_mutex_t* m)
  {
    int ret;
    do
    {
        ret = ::pthread_mutex_destroy(m);
    } while (ret == EINTR);
    return ret;
  }
  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_mutex_lock(pthread_mutex_t* m)
  {
    int ret;
    do
    {
        ret = ::pthread_mutex_lock(m);
    } while (ret == EINTR);
    return ret;
  }
  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_mutex_unlock(pthread_mutex_t* m)
  {
    int ret;
    do
    {
        ret = ::pthread_mutex_unlock(m);
    } while (ret == EINTR);
    return ret;
  }
#else
  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_mutex_destroy(pthread_mutex_t* m)
  {
    return ::pthread_mutex_destroy(m);
  }
  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_mutex_lock(pthread_mutex_t* m)
  {
    return ::pthread_mutex_lock(m);
  }
  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_mutex_unlock(pthread_mutex_t* m)
  {
    return ::pthread_mutex_unlock(m);
  }

#endif
  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_mutex_trylock(pthread_mutex_t* m)
  {
    return ::pthread_mutex_trylock(m);
  }

  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
  {
    return ::pthread_cond_wait(cond, mutex);
  }
  BOOST_FORCEINLINE BOOST_THREAD_DISABLE_THREAD_SAFETY_ANALYSIS
  int pthread_cond_signal(pthread_cond_t *cond)
  {
    return ::pthread_cond_signal(cond);
  }


}
    namespace pthread
    {
        class pthread_mutex_scoped_lock
        {
            pthread_mutex_t* m;
            bool locked;
        public:
            explicit pthread_mutex_scoped_lock(pthread_mutex_t* m_) BOOST_NOEXCEPT:
                m(m_),locked(true)
            {
                BOOST_VERIFY(!posix::pthread_mutex_lock(m));
            }
            void unlock() BOOST_NOEXCEPT
            {
                BOOST_VERIFY(!posix::pthread_mutex_unlock(m));
                locked=false;
            }
            void unlock_if_locked() BOOST_NOEXCEPT
            {
              if(locked)
              {
                  unlock();
              }
            }
            ~pthread_mutex_scoped_lock() BOOST_NOEXCEPT
            {
                if(locked)
                {
                    unlock();
                }
            }

        };

        class pthread_mutex_scoped_unlock
        {
            pthread_mutex_t* m;
        public:
            explicit pthread_mutex_scoped_unlock(pthread_mutex_t* m_) BOOST_NOEXCEPT:
                m(m_)
            {
                BOOST_VERIFY(!posix::pthread_mutex_unlock(m));
            }
            ~pthread_mutex_scoped_unlock() BOOST_NOEXCEPT
            {
                BOOST_VERIFY(!posix::pthread_mutex_lock(m));
            }

        };
    }
}

#include <boost/config/abi_suffix.hpp>

#endif
