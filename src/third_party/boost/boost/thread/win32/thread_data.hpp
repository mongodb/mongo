#ifndef BOOST_THREAD_PTHREAD_THREAD_DATA_HPP
#define BOOST_THREAD_PTHREAD_THREAD_DATA_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2008 Anthony Williams

#include <boost/thread/detail/config.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/thread/win32/thread_primitives.hpp>
#include <boost/thread/win32/thread_heap_alloc.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    namespace detail
    {
        struct thread_exit_callback_node;
        struct tss_data_node;

        struct thread_data_base;
        void intrusive_ptr_add_ref(thread_data_base * p);
        void intrusive_ptr_release(thread_data_base * p);

        struct BOOST_SYMBOL_VISIBLE thread_data_base
        {
            long count;
            detail::win32::handle_manager thread_handle;
            detail::win32::handle_manager interruption_handle;
            boost::detail::thread_exit_callback_node* thread_exit_callbacks;
            boost::detail::tss_data_node* tss_data;
            bool interruption_enabled;
            unsigned id;

            thread_data_base():
                count(0),thread_handle(detail::win32::invalid_handle_value),
                interruption_handle(create_anonymous_event(detail::win32::manual_reset_event,detail::win32::event_initially_reset)),
                thread_exit_callbacks(0),tss_data(0),
                interruption_enabled(true),
                id(0)
            {}
            virtual ~thread_data_base()
            {}

            friend void intrusive_ptr_add_ref(thread_data_base * p)
            {
                BOOST_INTERLOCKED_INCREMENT(&p->count);
            }

            friend void intrusive_ptr_release(thread_data_base * p)
            {
                if(!BOOST_INTERLOCKED_DECREMENT(&p->count))
                {
                    detail::heap_delete(p);
                }
            }

            void interrupt()
            {
                BOOST_VERIFY(detail::win32::SetEvent(interruption_handle)!=0);
            }

            typedef detail::win32::handle native_handle_type;

            virtual void run()=0;
        };

        typedef boost::intrusive_ptr<detail::thread_data_base> thread_data_ptr;

        struct BOOST_SYMBOL_VISIBLE timeout
        {
            unsigned long start;
            uintmax_t milliseconds;
            bool relative;
            boost::system_time abs_time;

            static unsigned long const max_non_infinite_wait=0xfffffffe;

            timeout(uintmax_t milliseconds_):
                start(win32::GetTickCount()),
                milliseconds(milliseconds_),
                relative(true),
                abs_time(boost::get_system_time())
            {}

            timeout(boost::system_time const& abs_time_):
                start(win32::GetTickCount()),
                milliseconds(0),
                relative(false),
                abs_time(abs_time_)
            {}

            struct BOOST_SYMBOL_VISIBLE remaining_time
            {
                bool more;
                unsigned long milliseconds;

                remaining_time(uintmax_t remaining):
                    more(remaining>max_non_infinite_wait),
                    milliseconds(more?max_non_infinite_wait:(unsigned long)remaining)
                {}
            };

            remaining_time remaining_milliseconds() const
            {
                if(is_sentinel())
                {
                    return remaining_time(win32::infinite);
                }
                else if(relative)
                {
                    unsigned long const now=win32::GetTickCount();
                    unsigned long const elapsed=now-start;
                    return remaining_time((elapsed<milliseconds)?(milliseconds-elapsed):0);
                }
                else
                {
                    system_time const now=get_system_time();
                    if(abs_time<=now)
                    {
                        return remaining_time(0);
                    }
                    return remaining_time((abs_time-now).total_milliseconds()+1);
                }
            }

            bool is_sentinel() const
            {
                return milliseconds==~uintmax_t(0);
            }


            static timeout sentinel()
            {
                return timeout(sentinel_type());
            }
        private:
            struct sentinel_type
            {};

            explicit timeout(sentinel_type):
                start(0),milliseconds(~uintmax_t(0)),relative(true)
            {}
        };

        inline uintmax_t pin_to_zero(intmax_t value)
        {
            return (value<0)?0u:(uintmax_t)value;
        }
    }

    namespace this_thread
    {
        void BOOST_THREAD_DECL yield();

        bool BOOST_THREAD_DECL interruptible_wait(detail::win32::handle handle_to_wait_for,detail::timeout target_time);
        inline void interruptible_wait(uintmax_t milliseconds)
        {
            interruptible_wait(detail::win32::invalid_handle_value,milliseconds);
        }
        inline BOOST_SYMBOL_VISIBLE void interruptible_wait(system_time const& abs_time)
        {
            interruptible_wait(detail::win32::invalid_handle_value,abs_time);
        }

        template<typename TimeDuration>
        inline BOOST_SYMBOL_VISIBLE void sleep(TimeDuration const& rel_time)
        {
            interruptible_wait(detail::pin_to_zero(rel_time.total_milliseconds()));
        }
        inline BOOST_SYMBOL_VISIBLE void sleep(system_time const& abs_time)
        {
            interruptible_wait(abs_time);
        }
    }

}

#include <boost/config/abi_suffix.hpp>

#endif
