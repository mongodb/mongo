// Copyright 2005-2009 Daniel James.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_FUNCTIONAL_HASH_DETAIL_LIMITS_HEADER
#define BOOST_FUNCTIONAL_HASH_DETAIL_LIMITS_HEADER

#include <limits>

namespace boost
{
    namespace hash_detail
    {
        template <class T>
        struct limits : std::numeric_limits<T> {};
    }
}

#endif // #ifndef BOOST_FUNCTIONAL_HASH_DETAIL_LIMITS_HEADER
