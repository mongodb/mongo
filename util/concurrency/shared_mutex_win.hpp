#ifndef BOOST_THREAD_WIN32_SHARED_MUTEX_HPP_MODIFIED
#define BOOST_THREAD_WIN32_SHARED_MUTEX_HPP_MODIFIED

//  (C) Copyright 2006-8 Anthony Williams
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

/* MongoDB : 
  Slightly modified boost file to not die above 127 pending writes
  Here is what changed (from boost 1.42.0 shared_mutex.hpp):
    1,2c1,2
    < #ifndef BOOST_THREAD_WIN32_SHARED_MUTEX_HPP
    < #define BOOST_THREAD_WIN32_SHARED_MUTEX_HPP
    ---
    > #ifndef BOOST_THREAD_WIN32_SHARED_MUTEX_HPP_MODIFIED
    > #define BOOST_THREAD_WIN32_SHARED_MUTEX_HPP_MODIFIED
    22c27
    <     class shared_mutex:
    ---
    >     class modified_shared_mutex:
    73c78
    <         shared_mutex():
    ---
    >         modified_shared_mutex():
    84c89
    <         ~shared_mutex()
    ---
    >         ~modified_shared_mutex()
    283a289,290
    >                         if( new_state.exclusive_waiting == 127 ) // the maximum already!
    >                             break;
*/

#include <boost/assert.hpp>
#include <boost/detail/interlocked.hpp>
#include <boost/thread/win32/thread_primitives.hpp>
#include <boost/static_assert.hpp>
#include <limits.h>
#include <boost/utility.hpp>
#include <boost/thread/thread_time.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    class modified_shared_mutex:
        private boost::noncopyable
    {
    private:
        struct state_data
        {
            unsigned shared_count:11,
                shared_waiting:11,
                exclusive:1,
                upgrade:1,
                exclusive_waiting:7,
                exclusive_waiting_blocked:1;

            friend bool operator==(state_data const& lhs,state_data const& rhs)
            {
                return *reinterpret_cast<unsigned const*>(&lhs)==*reinterpret_cast<unsigned const*>(&rhs);
            }
        };
        

        template<typename T>
        T interlocked_compare_exchange(T* target,T new_value,T comparand)
        {
            BOOST_STATIC_ASSERT(sizeof(T)==sizeof(long));
            long const res=BOOST_INTERLOCKED_COMPARE_EXCHANGE(reinterpret_cast<long*>(target),
                                                              *reinterpret_cast<long*>(&new_value),
                                                              *reinterpret_cast<long*>(&comparand));
            return *reinterpret_cast<T const*>(&res);
        }

        state_data state;
        detail::win32::handle semaphores[2];
        detail::win32::handle &unlock_sem;
        detail::win32::handle &exclusive_sem;
        detail::win32::handle upgrade_sem;

        void release_waiters(state_data old_state)
        {
            if(old_state.exclusive_waiting)
            {
                BOOST_VERIFY(detail::win32::ReleaseSemaphore(exclusive_sem,1,0)!=0);
            }
                        
            if(old_state.shared_waiting || old_state.exclusive_waiting)
            {
                BOOST_VERIFY(detail::win32::ReleaseSemaphore(unlock_sem,old_state.shared_waiting + (old_state.exclusive_waiting?1:0),0)!=0);
            }
        }
        

    public:
        modified_shared_mutex():
            unlock_sem(semaphores[0]),
            exclusive_sem(semaphores[1]) 
        {
            unlock_sem=detail::win32::create_anonymous_semaphore(0,LONG_MAX);
            exclusive_sem=detail::win32::create_anonymous_semaphore(0,LONG_MAX);
            upgrade_sem=detail::win32::create_anonymous_semaphore(0,LONG_MAX);
            state_data state_={0};
            state=state_;
        }

        ~modified_shared_mutex()
        {
            detail::win32::CloseHandle(upgrade_sem);
            detail::win32::CloseHandle(unlock_sem);
            detail::win32::CloseHandle(exclusive_sem);
        }

        bool try_lock_shared()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                if(!new_state.exclusive && !new_state.exclusive_waiting_blocked)
                {
                    ++new_state.shared_count;
                }
                
                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    break;
                }
                old_state=current_state;
            }
            return !(old_state.exclusive| old_state.exclusive_waiting_blocked);
        }

        void lock_shared()
        {
            BOOST_VERIFY(timed_lock_shared(::boost::detail::get_system_time_sentinel()));
        }

        template<typename TimeDuration>
        bool timed_lock_shared(TimeDuration const & relative_time)
        {
            return timed_lock_shared(get_system_time()+relative_time);
        }

        bool timed_lock_shared(boost::system_time const& wait_until)
        {
            for(;;)
            {
                state_data old_state=state;
                for(;;)
                {
                    state_data new_state=old_state;
                    if(new_state.exclusive || new_state.exclusive_waiting_blocked)
                    {
                        ++new_state.shared_waiting;
                    }
                    else
                    {
                        ++new_state.shared_count;
                    }

                    state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                    if(current_state==old_state)
                    {
                        break;
                    }
                    old_state=current_state;
                }

                if(!(old_state.exclusive| old_state.exclusive_waiting_blocked))
                {
                    return true;
                }
                    
                unsigned long const res=detail::win32::WaitForSingleObject(unlock_sem,::boost::detail::get_milliseconds_until(wait_until));
                if(res==detail::win32::timeout)
                {
                    for(;;)
                    {
                        state_data new_state=old_state;
                        if(new_state.exclusive || new_state.exclusive_waiting_blocked)
                        {
                            if(new_state.shared_waiting)
                            {
                                --new_state.shared_waiting;
                            }
                        }
                        else
                        {
                            ++new_state.shared_count;
                        }

                        state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                        if(current_state==old_state)
                        {
                            break;
                        }
                        old_state=current_state;
                    }

                    if(!(old_state.exclusive| old_state.exclusive_waiting_blocked))
                    {
                        return true;
                    }
                    return false;
                }
                
                BOOST_ASSERT(res==0);
            }
        }

        void unlock_shared()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                bool const last_reader=!--new_state.shared_count;
                
                if(last_reader)
                {
                    if(new_state.upgrade)
                    {
                        new_state.upgrade=false;
                        new_state.exclusive=true;
                    }
                    else
                    {
                        if(new_state.exclusive_waiting)
                        {
                            --new_state.exclusive_waiting;
                            new_state.exclusive_waiting_blocked=false;
                        }
                        new_state.shared_waiting=0;
                    }
                }
                
                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    if(last_reader)
                    {
                        if(old_state.upgrade)
                        {
                            BOOST_VERIFY(detail::win32::ReleaseSemaphore(upgrade_sem,1,0)!=0);
                        }
                        else
                        {
                            release_waiters(old_state);
                        }
                    }
                    break;
                }
                old_state=current_state;
            }
        }

        void lock()
        {
            BOOST_VERIFY(timed_lock(::boost::detail::get_system_time_sentinel()));
        }

        template<typename TimeDuration>
        bool timed_lock(TimeDuration const & relative_time)
        {
            return timed_lock(get_system_time()+relative_time);
        }

        bool try_lock()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                if(new_state.shared_count || new_state.exclusive)
                {
                    return false;
                }
                else
                {
                    new_state.exclusive=true;
                }
                
                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    break;
                }
                old_state=current_state;
            }
            return true;
        }


        bool timed_lock(boost::system_time const& wait_until)
        {
            for(;;)
            {
                state_data old_state=state;

                for(;;)
                {
                    state_data new_state=old_state;
                    if(new_state.shared_count || new_state.exclusive)
                    {
                        if( new_state.exclusive_waiting == 127 ) // the maximum already!
                            break;
                        ++new_state.exclusive_waiting;
                        new_state.exclusive_waiting_blocked=true;
                    }
                    else
                    {
                        new_state.exclusive=true;
                    }

                    state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                    if(current_state==old_state)
                    {
                        break;
                    }
                    old_state=current_state;
                }

                if(!old_state.shared_count && !old_state.exclusive)
                {
                    return true;
                }
                unsigned long const wait_res=detail::win32::WaitForMultipleObjects(2,semaphores,true,::boost::detail::get_milliseconds_until(wait_until));
                if(wait_res==detail::win32::timeout)
                {
                    for(;;)
                    {
                        state_data new_state=old_state;
                        if(new_state.shared_count || new_state.exclusive)
                        {
                            if(new_state.exclusive_waiting)
                            {
                                if(!--new_state.exclusive_waiting)
                                {
                                    new_state.exclusive_waiting_blocked=false;
                                }
                            }
                        }
                        else
                        {
                            new_state.exclusive=true;
                        }

                        state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                        if(current_state==old_state)
                        {
                            break;
                        }
                        old_state=current_state;
                    }
                    if(!old_state.shared_count && !old_state.exclusive)
                    {
                        return true;
                    }
                    return false;
                }
                BOOST_ASSERT(wait_res<2);
            }
        }

        void unlock()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                new_state.exclusive=false;
                if(new_state.exclusive_waiting)
                {
                    --new_state.exclusive_waiting;
                    new_state.exclusive_waiting_blocked=false;
                }
                new_state.shared_waiting=0;

                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    break;
                }
                old_state=current_state;
            }
            release_waiters(old_state);
        }

        void lock_upgrade()
        {
            for(;;)
            {
                state_data old_state=state;
                for(;;)
                {
                    state_data new_state=old_state;
                    if(new_state.exclusive || new_state.exclusive_waiting_blocked || new_state.upgrade)
                    {
                        ++new_state.shared_waiting;
                    }
                    else
                    {
                        ++new_state.shared_count;
                        new_state.upgrade=true;
                    }

                    state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                    if(current_state==old_state)
                    {
                        break;
                    }
                    old_state=current_state;
                }

                if(!(old_state.exclusive|| old_state.exclusive_waiting_blocked|| old_state.upgrade))
                {
                    return;
                }
                    
                BOOST_VERIFY(!detail::win32::WaitForSingleObject(unlock_sem,detail::win32::infinite));
            }
        }

        bool try_lock_upgrade()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                if(new_state.exclusive || new_state.exclusive_waiting_blocked || new_state.upgrade)
                {
                    return false;
                }
                else
                {
                    ++new_state.shared_count;
                    new_state.upgrade=true;
                }
                
                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    break;
                }
                old_state=current_state;
            }
            return true;
        }

        void unlock_upgrade()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                new_state.upgrade=false;
                bool const last_reader=!--new_state.shared_count;
                
                if(last_reader)
                {
                    if(new_state.exclusive_waiting)
                    {
                        --new_state.exclusive_waiting;
                        new_state.exclusive_waiting_blocked=false;
                    }
                    new_state.shared_waiting=0;
                }
                
                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    if(last_reader)
                    {
                        release_waiters(old_state);
                    }
                    break;
                }
                old_state=current_state;
            }
        }

        void unlock_upgrade_and_lock()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                bool const last_reader=!--new_state.shared_count;
                
                if(last_reader)
                {
                    new_state.upgrade=false;
                    new_state.exclusive=true;
                }
                
                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    if(!last_reader)
                    {
                        BOOST_VERIFY(!detail::win32::WaitForSingleObject(upgrade_sem,detail::win32::infinite));
                    }
                    break;
                }
                old_state=current_state;
            }
        }

        void unlock_and_lock_upgrade()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                new_state.exclusive=false;
                new_state.upgrade=true;
                ++new_state.shared_count;
                if(new_state.exclusive_waiting)
                {
                    --new_state.exclusive_waiting;
                    new_state.exclusive_waiting_blocked=false;
                }
                new_state.shared_waiting=0;

                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    break;
                }
                old_state=current_state;
            }
            release_waiters(old_state);
        }
        
        void unlock_and_lock_shared()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                new_state.exclusive=false;
                ++new_state.shared_count;
                if(new_state.exclusive_waiting)
                {
                    --new_state.exclusive_waiting;
                    new_state.exclusive_waiting_blocked=false;
                }
                new_state.shared_waiting=0;

                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    break;
                }
                old_state=current_state;
            }
            release_waiters(old_state);
        }
        
        void unlock_upgrade_and_lock_shared()
        {
            state_data old_state=state;
            for(;;)
            {
                state_data new_state=old_state;
                new_state.upgrade=false;
                if(new_state.exclusive_waiting)
                {
                    --new_state.exclusive_waiting;
                    new_state.exclusive_waiting_blocked=false;
                }
                new_state.shared_waiting=0;

                state_data const current_state=interlocked_compare_exchange(&state,new_state,old_state);
                if(current_state==old_state)
                {
                    break;
                }
                old_state=current_state;
            }
            release_waiters(old_state);
        }
        
    };
}

#include <boost/config/abi_suffix.hpp>

#endif
