#ifndef BOOST_THREAD_PTHREAD_ONCE_HPP
#define BOOST_THREAD_PTHREAD_ONCE_HPP

//  once.hpp
//
//  (C) Copyright 2007-8 Anthony Williams
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/thread/detail/config.hpp>
#include <boost/config.hpp>

#include <pthread.h>
#include <boost/assert.hpp>
#include <boost/thread/pthread/pthread_mutex_scoped_lock.hpp>
#include <boost/cstdint.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{

    struct once_flag
    {
        boost::uintmax_t epoch;
    };

    namespace detail
    {
        BOOST_THREAD_DECL boost::uintmax_t& get_once_per_thread_epoch();
        BOOST_THREAD_DECL extern boost::uintmax_t once_global_epoch;
        BOOST_THREAD_DECL extern pthread_mutex_t once_epoch_mutex;
        BOOST_THREAD_DECL extern pthread_cond_t once_epoch_cv;
    }

#define BOOST_ONCE_INITIAL_FLAG_VALUE 0
#define BOOST_ONCE_INIT {BOOST_ONCE_INITIAL_FLAG_VALUE}


    // Based on Mike Burrows fast_pthread_once algorithm as described in
    // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2444.html
    template<typename Function>
    void call_once(once_flag& flag,Function f)
    {
        static boost::uintmax_t const uninitialized_flag=BOOST_ONCE_INITIAL_FLAG_VALUE;
        static boost::uintmax_t const being_initialized=uninitialized_flag+1;
        boost::uintmax_t const epoch=flag.epoch;
        boost::uintmax_t& this_thread_epoch=detail::get_once_per_thread_epoch();

        if(epoch<this_thread_epoch)
        {
            pthread::pthread_mutex_scoped_lock lk(&detail::once_epoch_mutex);

            while(flag.epoch<=being_initialized)
            {
                if(flag.epoch==uninitialized_flag)
                {
                    flag.epoch=being_initialized;
#ifndef BOOST_NO_EXCEPTIONS
                    try
                    {
#endif
                        pthread::pthread_mutex_scoped_unlock relocker(&detail::once_epoch_mutex);
                        f();
#ifndef BOOST_NO_EXCEPTIONS
                    }
                    catch(...)
                    {
                        flag.epoch=uninitialized_flag;
                        BOOST_VERIFY(!pthread_cond_broadcast(&detail::once_epoch_cv));
                        throw;
                    }
#endif
                    flag.epoch=--detail::once_global_epoch;
                    BOOST_VERIFY(!pthread_cond_broadcast(&detail::once_epoch_cv));
                }
                else
                {
                    while(flag.epoch==being_initialized)
                    {
                        BOOST_VERIFY(!pthread_cond_wait(&detail::once_epoch_cv,&detail::once_epoch_mutex));
                    }
                }
            }
            this_thread_epoch=detail::once_global_epoch;
        }
    }
}

#include <boost/config/abi_suffix.hpp>

#endif
