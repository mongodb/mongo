#ifndef BOOST_BASIC_RECURSIVE_MUTEX_WIN32_HPP
#define BOOST_BASIC_RECURSIVE_MUTEX_WIN32_HPP

//  basic_recursive_mutex.hpp
//
//  (C) Copyright 2006-8 Anthony Williams
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/thread/win32/thread_primitives.hpp>
#include <boost/thread/win32/basic_timed_mutex.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    namespace detail
    {
        template<typename underlying_mutex_type>
        struct basic_recursive_mutex_impl
        {
            long recursion_count;
            long locking_thread_id;
            underlying_mutex_type mutex;

            void initialize()
            {
                recursion_count=0;
                locking_thread_id=0;
                mutex.initialize();
            }

            void destroy()
            {
                mutex.destroy();
            }

            bool try_lock()
            {
                long const current_thread_id=win32::GetCurrentThreadId();
                return try_recursive_lock(current_thread_id) || try_basic_lock(current_thread_id);
            }

            void lock()
            {
                long const current_thread_id=win32::GetCurrentThreadId();
                if(!try_recursive_lock(current_thread_id))
                {
                    mutex.lock();
                    BOOST_INTERLOCKED_EXCHANGE(&locking_thread_id,current_thread_id);
                    recursion_count=1;
                }
            }
            bool timed_lock(::boost::system_time const& target)
            {
                long const current_thread_id=win32::GetCurrentThreadId();
                return try_recursive_lock(current_thread_id) || try_timed_lock(current_thread_id,target);
            }
            template<typename Duration>
            bool timed_lock(Duration const& timeout)
            {
                return timed_lock(get_system_time()+timeout);
            }

            void unlock()
            {
                if(!--recursion_count)
                {
                    BOOST_INTERLOCKED_EXCHANGE(&locking_thread_id,0);
                    mutex.unlock();
                }
            }

        private:
            bool try_recursive_lock(long current_thread_id)
            {
                if(::boost::detail::interlocked_read_acquire(&locking_thread_id)==current_thread_id)
                {
                    ++recursion_count;
                    return true;
                }
                return false;
            }

            bool try_basic_lock(long current_thread_id)
            {
                if(mutex.try_lock())
                {
                    BOOST_INTERLOCKED_EXCHANGE(&locking_thread_id,current_thread_id);
                    recursion_count=1;
                    return true;
                }
                return false;
            }

            bool try_timed_lock(long current_thread_id,::boost::system_time const& target)
            {
                if(mutex.timed_lock(target))
                {
                    BOOST_INTERLOCKED_EXCHANGE(&locking_thread_id,current_thread_id);
                    recursion_count=1;
                    return true;
                }
                return false;
            }

        };

        typedef basic_recursive_mutex_impl<basic_timed_mutex> basic_recursive_mutex;
        typedef basic_recursive_mutex_impl<basic_timed_mutex> basic_recursive_timed_mutex;
    }
}

#define BOOST_BASIC_RECURSIVE_MUTEX_INITIALIZER {0}

#include <boost/config/abi_suffix.hpp>

#endif
