#ifndef BOOST_BASIC_TIMED_MUTEX_WIN32_HPP
#define BOOST_BASIC_TIMED_MUTEX_WIN32_HPP

//  basic_timed_mutex_win32.hpp
//
//  (C) Copyright 2006-8 Anthony Williams
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/assert.hpp>
#include <boost/thread/win32/thread_primitives.hpp>
#include <boost/thread/win32/interlocked_read.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/detail/interlocked.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    namespace detail
    {
        struct basic_timed_mutex
        {
            BOOST_STATIC_CONSTANT(unsigned char,lock_flag_bit=31);
            BOOST_STATIC_CONSTANT(unsigned char,event_set_flag_bit=30);
            BOOST_STATIC_CONSTANT(long,lock_flag_value=1<<lock_flag_bit);
            BOOST_STATIC_CONSTANT(long,event_set_flag_value=1<<event_set_flag_bit);
            long active_count;
            void* event;

            void initialize()
            {
                active_count=0;
                event=0;
            }

            void destroy()
            {
#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4312)
#endif
                void* const old_event=BOOST_INTERLOCKED_EXCHANGE_POINTER(&event,0);
#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
                if(old_event)
                {
                    win32::CloseHandle(old_event);
                }
            }


            bool try_lock()
            {
                return !win32::interlocked_bit_test_and_set(&active_count,lock_flag_bit);
            }

            void lock()
            {
                if(try_lock())
                {
                    return;
                }
                long old_count=active_count;
                mark_waiting_and_try_lock(old_count);

                if(old_count&lock_flag_value)
                {
                    bool lock_acquired=false;
                    void* const sem=get_event();

                    do
                    {
                        BOOST_VERIFY(win32::WaitForSingleObject(
                                         sem,::boost::detail::win32::infinite)==0);
                        clear_waiting_and_try_lock(old_count);
                        lock_acquired=!(old_count&lock_flag_value);
                    }
                    while(!lock_acquired);
                }
            }
            void mark_waiting_and_try_lock(long& old_count)
            {
                for(;;)
                {
                    long const new_count=(old_count&lock_flag_value)?(old_count+1):(old_count|lock_flag_value);
                    long const current=BOOST_INTERLOCKED_COMPARE_EXCHANGE(&active_count,new_count,old_count);
                    if(current==old_count)
                    {
                        break;
                    }
                    old_count=current;
                }
            }

            void clear_waiting_and_try_lock(long& old_count)
            {
                old_count&=~lock_flag_value;
                old_count|=event_set_flag_value;
                for(;;)
                {
                    long const new_count=((old_count&lock_flag_value)?old_count:((old_count-1)|lock_flag_value))&~event_set_flag_value;
                    long const current=BOOST_INTERLOCKED_COMPARE_EXCHANGE(&active_count,new_count,old_count);
                    if(current==old_count)
                    {
                        break;
                    }
                    old_count=current;
                }
            }


            bool timed_lock(::boost::system_time const& wait_until)
            {
                if(try_lock())
                {
                    return true;
                }
                long old_count=active_count;
                mark_waiting_and_try_lock(old_count);

                if(old_count&lock_flag_value)
                {
                    bool lock_acquired=false;
                    void* const sem=get_event();

                    do
                    {
                        if(win32::WaitForSingleObject(sem,::boost::detail::get_milliseconds_until(wait_until))!=0)
                        {
                            BOOST_INTERLOCKED_DECREMENT(&active_count);
                            return false;
                        }
                        clear_waiting_and_try_lock(old_count);
                        lock_acquired=!(old_count&lock_flag_value);
                    }
                    while(!lock_acquired);
                }
                return true;
            }

            template<typename Duration>
            bool timed_lock(Duration const& timeout)
            {
                return timed_lock(get_system_time()+timeout);
            }

            bool timed_lock(boost::xtime const& timeout)
            {
                return timed_lock(system_time(timeout));
            }

            void unlock()
            {
                long const offset=lock_flag_value;
                long const old_count=BOOST_INTERLOCKED_EXCHANGE_ADD(&active_count,lock_flag_value);
                if(!(old_count&event_set_flag_value) && (old_count>offset))
                {
                    if(!win32::interlocked_bit_test_and_set(&active_count,event_set_flag_bit))
                    {
                        win32::SetEvent(get_event());
                    }
                }
            }

        private:
            void* get_event()
            {
                void* current_event=::boost::detail::interlocked_read_acquire(&event);

                if(!current_event)
                {
                    void* const new_event=win32::create_anonymous_event(win32::auto_reset_event,win32::event_initially_reset);
#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4311)
#pragma warning(disable:4312)
#endif
                    void* const old_event=BOOST_INTERLOCKED_COMPARE_EXCHANGE_POINTER(&event,new_event,0);
#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
                    if(old_event!=0)
                    {
                        win32::CloseHandle(new_event);
                        return old_event;
                    }
                    else
                    {
                        return new_event;
                    }
                }
                return current_event;
            }

        };

    }
}

#define BOOST_BASIC_TIMED_MUTEX_INITIALIZER {0}

#include <boost/config/abi_suffix.hpp>

#endif
