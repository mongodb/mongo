#ifndef BOOST_RECURSIVE_MUTEX_WIN32_HPP
#define BOOST_RECURSIVE_MUTEX_WIN32_HPP

//  recursive_mutex.hpp
//
//  (C) Copyright 2006-7 Anthony Williams
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)


#include <boost/utility.hpp>
#include <boost/thread/win32/basic_recursive_mutex.hpp>
#include <boost/thread/exceptions.hpp>
#include <boost/thread/locks.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
    class recursive_mutex:
        public ::boost::detail::basic_recursive_mutex
    {
    private:
        recursive_mutex(recursive_mutex const&);
        recursive_mutex& operator=(recursive_mutex const&);
    public:
        recursive_mutex()
        {
            ::boost::detail::basic_recursive_mutex::initialize();
        }
        ~recursive_mutex()
        {
            ::boost::detail::basic_recursive_mutex::destroy();
        }

        typedef unique_lock<recursive_mutex> scoped_lock;
        typedef detail::try_lock_wrapper<recursive_mutex> scoped_try_lock;
    };

    typedef recursive_mutex recursive_try_mutex;

    class recursive_timed_mutex:
        public ::boost::detail::basic_recursive_timed_mutex
    {
    private:
        recursive_timed_mutex(recursive_timed_mutex const&);
        recursive_timed_mutex& operator=(recursive_timed_mutex const&);
    public:
        recursive_timed_mutex()
        {
            ::boost::detail::basic_recursive_timed_mutex::initialize();
        }
        ~recursive_timed_mutex()
        {
            ::boost::detail::basic_recursive_timed_mutex::destroy();
        }

        typedef unique_lock<recursive_timed_mutex> scoped_timed_lock;
        typedef detail::try_lock_wrapper<recursive_timed_mutex> scoped_try_lock;
        typedef scoped_timed_lock scoped_lock;
    };
}

#include <boost/config/abi_suffix.hpp>

#endif
