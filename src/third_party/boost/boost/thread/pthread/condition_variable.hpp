#ifndef BOOST_THREAD_CONDITION_VARIABLE_PTHREAD_HPP
#define BOOST_THREAD_CONDITION_VARIABLE_PTHREAD_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007-10 Anthony Williams

#include <boost/thread/pthread/timespec.hpp>
#include <boost/thread/pthread/pthread_mutex_scoped_lock.hpp>
#include <boost/thread/pthread/thread_data.hpp>
#include <boost/thread/pthread/condition_variable_fwd.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    namespace this_thread
    {
        void BOOST_THREAD_DECL interruption_point();
    }

    namespace thread_cv_detail
    {
        template<typename MutexType>
        struct lock_on_exit
        {
            MutexType* m;

            lock_on_exit():
                m(0)
            {}

            void activate(MutexType& m_)
            {
                m_.unlock();
                m=&m_;
            }
            ~lock_on_exit()
            {
                if(m)
                {
                    m->lock();
                }
           }
        };
    }

    inline void condition_variable::wait(unique_lock<mutex>& m)
    {
        int res=0;
        {
            thread_cv_detail::lock_on_exit<unique_lock<mutex> > guard;
            detail::interruption_checker check_for_interruption(&internal_mutex,&cond);
            guard.activate(m);
            do {
              res = pthread_cond_wait(&cond,&internal_mutex);
            } while (res == EINTR);
        }
        this_thread::interruption_point();
        if(res)
        {
            boost::throw_exception(condition_error());
        }
    }

    inline bool condition_variable::timed_wait(unique_lock<mutex>& m,boost::system_time const& wait_until)
    {
        thread_cv_detail::lock_on_exit<unique_lock<mutex> > guard;
        int cond_res;
        {
            detail::interruption_checker check_for_interruption(&internal_mutex,&cond);
            guard.activate(m);
            struct timespec const timeout=detail::get_timespec(wait_until);
            cond_res=pthread_cond_timedwait(&cond,&internal_mutex,&timeout);
        }
        this_thread::interruption_point();
        if(cond_res==ETIMEDOUT)
        {
            return false;
        }
        if(cond_res)
        {
            boost::throw_exception(condition_error());
        }
        return true;
    }

    inline void condition_variable::notify_one()
    {
        boost::pthread::pthread_mutex_scoped_lock internal_lock(&internal_mutex);
        BOOST_VERIFY(!pthread_cond_signal(&cond));
    }

    inline void condition_variable::notify_all()
    {
        boost::pthread::pthread_mutex_scoped_lock internal_lock(&internal_mutex);
        BOOST_VERIFY(!pthread_cond_broadcast(&cond));
    }

    class condition_variable_any
    {
        pthread_mutex_t internal_mutex;
        pthread_cond_t cond;

        condition_variable_any(condition_variable_any&);
        condition_variable_any& operator=(condition_variable_any&);

    public:
        condition_variable_any()
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
        ~condition_variable_any()
        {
            BOOST_VERIFY(!pthread_mutex_destroy(&internal_mutex));
            BOOST_VERIFY(!pthread_cond_destroy(&cond));
        }

        template<typename lock_type>
        void wait(lock_type& m)
        {
            int res=0;
            {
                thread_cv_detail::lock_on_exit<lock_type> guard;
                detail::interruption_checker check_for_interruption(&internal_mutex,&cond);
                guard.activate(m);
                res=pthread_cond_wait(&cond,&internal_mutex);
            }
            this_thread::interruption_point();
            if(res)
            {
                boost::throw_exception(condition_error());
            }
        }

        template<typename lock_type,typename predicate_type>
        void wait(lock_type& m,predicate_type pred)
        {
            while(!pred()) wait(m);
        }

        template<typename lock_type>
        bool timed_wait(lock_type& m,boost::system_time const& wait_until)
        {
            struct timespec const timeout=detail::get_timespec(wait_until);
            int res=0;
            {
                thread_cv_detail::lock_on_exit<lock_type> guard;
                detail::interruption_checker check_for_interruption(&internal_mutex,&cond);
                guard.activate(m);
                res=pthread_cond_timedwait(&cond,&internal_mutex,&timeout);
            }
            this_thread::interruption_point();
            if(res==ETIMEDOUT)
            {
                return false;
            }
            if(res)
            {
                boost::throw_exception(condition_error());
            }
            return true;
        }
        template<typename lock_type>
        bool timed_wait(lock_type& m,xtime const& wait_until)
        {
            return timed_wait(m,system_time(wait_until));
        }

        template<typename lock_type,typename duration_type>
        bool timed_wait(lock_type& m,duration_type const& wait_duration)
        {
            return timed_wait(m,get_system_time()+wait_duration);
        }

        template<typename lock_type,typename predicate_type>
        bool timed_wait(lock_type& m,boost::system_time const& wait_until,predicate_type pred)
        {
            while (!pred())
            {
                if(!timed_wait(m, wait_until))
                    return pred();
            }
            return true;
        }

        template<typename lock_type,typename predicate_type>
        bool timed_wait(lock_type& m,xtime const& wait_until,predicate_type pred)
        {
            return timed_wait(m,system_time(wait_until),pred);
        }

        template<typename lock_type,typename duration_type,typename predicate_type>
        bool timed_wait(lock_type& m,duration_type const& wait_duration,predicate_type pred)
        {
            return timed_wait(m,get_system_time()+wait_duration,pred);
        }

        void notify_one()
        {
            boost::pthread::pthread_mutex_scoped_lock internal_lock(&internal_mutex);
            BOOST_VERIFY(!pthread_cond_signal(&cond));
        }

        void notify_all()
        {
            boost::pthread::pthread_mutex_scoped_lock internal_lock(&internal_mutex);
            BOOST_VERIFY(!pthread_cond_broadcast(&cond));
        }
    };

}

#include <boost/config/abi_suffix.hpp>

#endif
