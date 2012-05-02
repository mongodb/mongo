// Copyright (C) 2007 Anthony Williams
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#define __STDC_CONSTANT_MACROS
#include <boost/thread/once.hpp>
#include <boost/assert.hpp>
#include <pthread.h>
#include <stdlib.h>

namespace boost
{
    namespace detail
    {
        BOOST_THREAD_DECL boost::uintmax_t once_global_epoch=UINTMAX_C(~0);
        BOOST_THREAD_DECL pthread_mutex_t once_epoch_mutex=PTHREAD_MUTEX_INITIALIZER;
        BOOST_THREAD_DECL pthread_cond_t once_epoch_cv = PTHREAD_COND_INITIALIZER;

        namespace
        {
            pthread_key_t epoch_tss_key;
            pthread_once_t epoch_tss_key_flag=PTHREAD_ONCE_INIT;
            
            extern "C"
            {
                static void delete_epoch_tss_data(void* data)
                {
                    free(data);
                }

                static void create_epoch_tss_key()
                {
                    BOOST_VERIFY(!pthread_key_create(&epoch_tss_key,delete_epoch_tss_data));
                }
            }
        }
        
        boost::uintmax_t& get_once_per_thread_epoch()
        {
            BOOST_VERIFY(!pthread_once(&epoch_tss_key_flag,create_epoch_tss_key));
            void* data=pthread_getspecific(epoch_tss_key);
            if(!data)
            {
                data=malloc(sizeof(boost::uintmax_t));
                BOOST_VERIFY(!pthread_setspecific(epoch_tss_key,data));
                *static_cast<boost::uintmax_t*>(data)=UINTMAX_C(~0);
            }
            return *static_cast<boost::uintmax_t*>(data);
        }
    }
    
}
