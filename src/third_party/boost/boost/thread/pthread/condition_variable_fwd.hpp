#ifndef BOOST_THREAD_PTHREAD_CONDITION_VARIABLE_FWD_HPP
#define BOOST_THREAD_PTHREAD_CONDITION_VARIABLE_FWD_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007-8 Anthony Williams

#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>
#include <pthread.h>
#include <boost/thread/mutex.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/thread/xtime.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    class condition_variable
    {
    private:
        pthread_mutex_t internal_mutex;
        pthread_cond_t cond;

        condition_variable(condition_variable&);
        condition_variable& operator=(condition_variable&);

    public:
        condition_variable()
        {
            int const res=pthread_mutex_init(&internal_mutex,NULL);
            if(res)
            {
                boost::throw_exception(thread_resource_error());
            }
            int const res2=pthread_cond_init(&cond,NULL);
            if(res2)
            {
                BOOST_VERIFY(!pthread_mutex_destroy(&internal_mutex));
                boost::throw_exception(thread_resource_error());
            }
        }
        ~condition_variable()
        {
            BOOST_VERIFY(!pthread_mutex_destroy(&internal_mutex));
            int ret;
            do {
              ret = pthread_cond_destroy(&cond);
            } while (ret == EINTR);
            BOOST_VERIFY(!ret);
        }

        void wait(unique_lock<mutex>& m);

        template<typename predicate_type>
        void wait(unique_lock<mutex>& m,predicate_type pred)
        {
            while(!pred()) wait(m);
        }

        inline bool timed_wait(unique_lock<mutex>& m,
                               boost::system_time const& wait_until);
        bool timed_wait(unique_lock<mutex>& m,xtime const& wait_until)
        {
            return timed_wait(m,system_time(wait_until));
        }

        template<typename duration_type>
        bool timed_wait(unique_lock<mutex>& m,duration_type const& wait_duration)
        {
            return timed_wait(m,get_system_time()+wait_duration);
        }

        template<typename predicate_type>
        bool timed_wait(unique_lock<mutex>& m,boost::system_time const& wait_until,predicate_type pred)
        {
            while (!pred())
            {
                if(!timed_wait(m, wait_until))
                    return pred();
            }
            return true;
        }

        template<typename predicate_type>
        bool timed_wait(unique_lock<mutex>& m,xtime const& wait_until,predicate_type pred)
        {
            return timed_wait(m,system_time(wait_until),pred);
        }

        template<typename duration_type,typename predicate_type>
        bool timed_wait(unique_lock<mutex>& m,duration_type const& wait_duration,predicate_type pred)
        {
            return timed_wait(m,get_system_time()+wait_duration,pred);
        }

        typedef pthread_cond_t* native_handle_type;
        native_handle_type native_handle()
        {
            return &cond;
        }

        void notify_one();
        void notify_all();
    };
}

#include <boost/config/abi_suffix.hpp>

#endif
