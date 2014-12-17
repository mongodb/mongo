#ifndef BOOST_THREAD_PTHREAD_TIMESPEC_HPP
#define BOOST_THREAD_PTHREAD_TIMESPEC_HPP
//  (C) Copyright 2007-8 Anthony Williams 
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/thread/thread_time.hpp>
#include <boost/date_time/posix_time/conversion.hpp>
#include <pthread.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    namespace detail
    {
        inline struct timespec get_timespec(boost::system_time const& abs_time)
        {
            struct timespec timeout={0,0};
            boost::posix_time::time_duration const time_since_epoch=abs_time-boost::posix_time::from_time_t(0);
            
            timeout.tv_sec=time_since_epoch.total_seconds();
            timeout.tv_nsec=(long)(time_since_epoch.fractional_seconds()*(1000000000l/time_since_epoch.ticks_per_second()));
            return timeout;
        }
    }
}

#include <boost/config/abi_suffix.hpp>

#endif
