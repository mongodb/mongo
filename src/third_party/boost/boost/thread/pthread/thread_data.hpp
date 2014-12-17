#ifndef BOOST_THREAD_PTHREAD_THREAD_DATA_HPP
#define BOOST_THREAD_PTHREAD_THREAD_DATA_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007 Anthony Williams

#include <boost/thread/detail/config.hpp>
#include <boost/thread/exceptions.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/optional.hpp>
#include <pthread.h>
#include <boost/assert.hpp>
#include <boost/thread/pthread/condition_variable_fwd.hpp>
#include <map>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    class thread;

    namespace detail
    {
        struct tss_cleanup_function;
        struct thread_exit_callback_node;
        struct tss_data_node
        {
            boost::shared_ptr<boost::detail::tss_cleanup_function> func;
            void* value;

            tss_data_node(boost::shared_ptr<boost::detail::tss_cleanup_function> func_,
                          void* value_):
                func(func_),value(value_)
            {}
        };

        struct thread_data_base;
        typedef boost::shared_ptr<thread_data_base> thread_data_ptr;

        struct BOOST_THREAD_DECL thread_data_base:
            enable_shared_from_this<thread_data_base>
        {
            thread_data_ptr self;
            pthread_t thread_handle;
            boost::mutex data_mutex;
            boost::condition_variable done_condition;
            boost::mutex sleep_mutex;
            boost::condition_variable sleep_condition;
            bool done;
            bool join_started;
            bool joined;
            boost::detail::thread_exit_callback_node* thread_exit_callbacks;
            std::map<void const*,boost::detail::tss_data_node> tss_data;
            bool interrupt_enabled;
            bool interrupt_requested;
            pthread_mutex_t* cond_mutex;
            pthread_cond_t* current_cond;

            thread_data_base():
                done(false),join_started(false),joined(false),
                thread_exit_callbacks(0),
                interrupt_enabled(true),
                interrupt_requested(false),
                current_cond(0)
            {}
            virtual ~thread_data_base();

            typedef pthread_t native_handle_type;

            virtual void run()=0;
        };

        BOOST_THREAD_DECL thread_data_base* get_current_thread_data();

        class interruption_checker
        {
            thread_data_base* const thread_info;
            pthread_mutex_t* m;
            bool set;

            void check_for_interruption()
            {
                if(thread_info->interrupt_requested)
                {
                    thread_info->interrupt_requested=false;
                    throw thread_interrupted();
                }
            }

            void operator=(interruption_checker&);
        public:
            explicit interruption_checker(pthread_mutex_t* cond_mutex,pthread_cond_t* cond):
                thread_info(detail::get_current_thread_data()),m(cond_mutex),
                set(thread_info && thread_info->interrupt_enabled)
            {
                if(set)
                {
                    lock_guard<mutex> guard(thread_info->data_mutex);
                    check_for_interruption();
                    thread_info->cond_mutex=cond_mutex;
                    thread_info->current_cond=cond;
                    BOOST_VERIFY(!pthread_mutex_lock(m));
                }
                else
                {
                    BOOST_VERIFY(!pthread_mutex_lock(m));
                }
            }
            ~interruption_checker()
            {
                if(set)
                {
                    BOOST_VERIFY(!pthread_mutex_unlock(m));
                    lock_guard<mutex> guard(thread_info->data_mutex);
                    thread_info->cond_mutex=NULL;
                    thread_info->current_cond=NULL;
                }
                else
                {
                    BOOST_VERIFY(!pthread_mutex_unlock(m));
                }
            }
        };
    }

    namespace this_thread
    {
        void BOOST_THREAD_DECL yield();

#ifdef __DECXXX
        /// Workaround of DECCXX issue of incorrect template substitution
        template<typename TimeDuration>
        inline void sleep(TimeDuration const& rel_time)
        {
            this_thread::sleep(get_system_time()+rel_time);
        }

        template<>
        void BOOST_THREAD_DECL sleep(system_time const& abs_time);
#else
        void BOOST_THREAD_DECL sleep(system_time const& abs_time);

        template<typename TimeDuration>
        inline BOOST_SYMBOL_VISIBLE void sleep(TimeDuration const& rel_time)
        {
            this_thread::sleep(get_system_time()+rel_time);
        }
#endif
    }
}

#include <boost/config/abi_suffix.hpp>

#endif
