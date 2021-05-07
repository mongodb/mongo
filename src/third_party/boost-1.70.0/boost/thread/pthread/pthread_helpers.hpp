#ifndef BOOST_THREAD_PTHREAD_PTHREAD_HELPERS_HPP
#define BOOST_THREAD_PTHREAD_PTHREAD_HELPERS_HPP
// Copyright (C) 2017
// Vicente J. Botet Escriba
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/thread/detail/config.hpp>
#include <pthread.h>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    namespace pthread
    {
      inline int cond_init(pthread_cond_t& cond) {

  #ifdef BOOST_THREAD_INTERNAL_CLOCK_IS_MONO
              pthread_condattr_t attr;
              int res = pthread_condattr_init(&attr);
              if (res)
              {
                return res;
              }
              pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
              res=pthread_cond_init(&cond,&attr);
              pthread_condattr_destroy(&attr);
              return res;
  #else
              return pthread_cond_init(&cond,NULL);
  #endif

      }
    }
}

#include <boost/config/abi_suffix.hpp>

#endif
