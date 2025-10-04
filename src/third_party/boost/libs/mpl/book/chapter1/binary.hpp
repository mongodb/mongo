//
// Copyright 2005 David Abrahams and Aleksey Gurtovoy. Distributed
// under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#include "boost/mpl/long.hpp"
#include "boost/mpl/alias.hpp"

template< long n > struct binary
    : mpl::long_< ( binary< n / 10 >::value << 1 ) + n % 10 >
{
};

template<> struct binary<0>
    : mpl::long_<0>
{
};
