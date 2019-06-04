/*
 *             Copyright Andrey Semashev 2017.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */

// Test if we can include system headers with -D_XOPEN_SOURCE=600
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600

#include <unistd.h>

int main(int, char*[])
{
    return 0;
}
