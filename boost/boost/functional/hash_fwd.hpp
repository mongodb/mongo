
//  Copyright Daniel James 2005-2006. Use, modification, and distribution are
//  subject to the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  Based on Peter Dimov's proposal
//  http://www.open-std.org/JTC1/SC22/WG21/docs/papers/2005/n1756.pdf
//  issue 6.18. 

#if !defined(BOOST_FUNCTIONAL_HASH_FWD_HPP)
#define BOOST_FUNCTIONAL_HASH_FWD_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp>
#include <cstddef>
#include <boost/detail/workaround.hpp>

namespace boost
{
    template <class T> struct hash;

#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
    template <class T> void hash_combine(std::size_t& seed, T& v);
#else
    template <class T> void hash_combine(std::size_t& seed, T const& v);
#endif

    template <class It> std::size_t hash_range(It, It);
    template <class It> void hash_range(std::size_t&, It, It);

#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x551))
    template <class T> inline std::size_t hash_range(T*, T*);
    template <class T> inline void hash_range(std::size_t&, T*, T*);
#endif
}

#endif
