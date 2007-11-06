// Copyright (C) 2001-2003
// William E. Kempf
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_ONCE_WEK080101_HPP
#define BOOST_ONCE_WEK080101_HPP

#include <boost/thread/detail/config.hpp>

#if defined(BOOST_HAS_PTHREADS)
#   include <pthread.h>
#endif

namespace boost {

#if defined(BOOST_HAS_PTHREADS)

typedef pthread_once_t once_flag;
#define BOOST_ONCE_INIT PTHREAD_ONCE_INIT

#elif (defined(BOOST_HAS_WINTHREADS) || defined(BOOST_HAS_MPTASKS))

typedef long once_flag;
#define BOOST_ONCE_INIT 0

#endif

void BOOST_THREAD_DECL call_once(void (*func)(), once_flag& flag);

} // namespace boost

// Change Log:
//   1 Aug 01  WEKEMPF Initial version.

#endif // BOOST_ONCE_WEK080101_HPP
