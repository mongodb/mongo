//
// detail/thread.hpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_THREAD_HPP
#define BOOST_ASIO_DETAIL_THREAD_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if !defined(BOOST_HAS_THREADS) || defined(BOOST_ASIO_DISABLE_THREADS)
# include <boost/asio/detail/null_thread.hpp>
#elif defined(BOOST_WINDOWS)
# if defined(UNDER_CE)
#  include <boost/asio/detail/wince_thread.hpp>
# else
#  include <boost/asio/detail/win_thread.hpp>
# endif
#elif defined(BOOST_HAS_PTHREADS)
# include <boost/asio/detail/posix_thread.hpp>
#else
# error Only Windows and POSIX are supported!
#endif

namespace boost {
namespace asio {
namespace detail {

#if !defined(BOOST_HAS_THREADS) || defined(BOOST_ASIO_DISABLE_THREADS)
typedef null_thread thread;
#elif defined(BOOST_WINDOWS)
# if defined(UNDER_CE)
typedef wince_thread thread;
# else
typedef win_thread thread;
# endif
#elif defined(BOOST_HAS_PTHREADS)
typedef posix_thread thread;
#endif

} // namespace detail
} // namespace asio
} // namespace boost

#endif // BOOST_ASIO_DETAIL_THREAD_HPP
