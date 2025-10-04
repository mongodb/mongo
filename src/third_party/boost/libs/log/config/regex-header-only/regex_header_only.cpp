/*
 *             Copyright Andrey Semashev 2023.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */

#include <boost/regex/config.hpp>

#if defined(BOOST_REGEX_CXX03)
#error Boost.Log: Boost.Regex is in C++03 mode and is not header-only
#endif

int main(int, char*[])
{
    return 0;
}
