#ifndef BOOST_THREAD_DETAIL_INTERLOCKED_READ_WIN32_HPP
#define BOOST_THREAD_DETAIL_INTERLOCKED_READ_WIN32_HPP

//  interlocked_read_win32.hpp
//
//  (C) Copyright 2005-8 Anthony Williams 
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/detail/interlocked.hpp>

#include <boost/config/abi_prefix.hpp>

#ifdef BOOST_MSVC

extern "C" void _ReadWriteBarrier(void);
#pragma intrinsic(_ReadWriteBarrier)

namespace boost
{
    namespace detail
    {
        inline long interlocked_read_acquire(long volatile* x)
        {
            long const res=*x;
            _ReadWriteBarrier();
            return res;
        }
        inline void* interlocked_read_acquire(void* volatile* x)
        {
            void* const res=*x;
            _ReadWriteBarrier();
            return res;
        }

        inline void interlocked_write_release(long volatile* x,long value)
        {
            _ReadWriteBarrier();
            *x=value;
        }
        inline void interlocked_write_release(void* volatile* x,void* value)
        {
            _ReadWriteBarrier();
            *x=value;
        }
    }
}

#else

namespace boost
{
    namespace detail
    {
        inline long interlocked_read_acquire(long volatile* x)
        {
            return BOOST_INTERLOCKED_COMPARE_EXCHANGE(x,0,0);
        }
        inline void* interlocked_read_acquire(void* volatile* x)
        {
            return BOOST_INTERLOCKED_COMPARE_EXCHANGE_POINTER(x,0,0);
        }
        inline void interlocked_write_release(long volatile* x,long value)
        {
            BOOST_INTERLOCKED_EXCHANGE(x,value);
        }
        inline void interlocked_write_release(void* volatile* x,void* value)
        {
            BOOST_INTERLOCKED_EXCHANGE_POINTER(x,value);
        }
    }
}

#endif

#include <boost/config/abi_suffix.hpp>

#endif
