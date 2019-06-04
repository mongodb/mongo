/*
 *             Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */

#include <boost/atomic.hpp>

#if !defined(BOOST_ATOMIC_INT32_LOCK_FREE) || (BOOST_ATOMIC_INT32_LOCK_FREE+0) != 2
#error Boost.Log: Native 32-bit atomic operations are required but not supported by Boost.Atomic on the target platform
#endif

int main(int, char*[])
{
    return 0;
}
