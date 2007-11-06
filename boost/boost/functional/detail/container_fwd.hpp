
//  Copyright Daniel James 2005-2006. Use, modification, and distribution are
//  subject to the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(BOOST_DETAIL_CONTAINER_FWD_HPP)
#define BOOST_DETAIL_CONTAINER_FWD_HPP

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#if BOOST_WORKAROUND(__GNUC__, < 3) && !defined(__SGI_STL_PORT) && !defined(_STLPORT_VERSION)
#define BOOST_HASH_CHAR_TRAITS string_char_traits
#else
#define BOOST_HASH_CHAR_TRAITS char_traits
#endif

#if (defined(__GLIBCXX__) && defined(_GLIBCXX_DEBUG)) \
    || BOOST_WORKAROUND(__BORLANDC__, > 0x551) \
    || BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x842)) \
    || (defined(__SGI_STL_PORT) || defined(_STLPORT_VERSION))

#include <deque>
#include <list>
#include <vector>
#include <map>
#include <set>
#include <bitset>
#include <string>
#include <complex>

#else

#include <cstddef>

#if !defined(__SGI_STL_PORT) && !defined(_STLPORT_VERSION) && \
        defined(__STL_CONFIG_H)

#define BOOST_CONTAINER_FWD_BAD_BITSET

#if !defined(__STL_NON_TYPE_TMPL_PARAM_BUG)
#define BOOST_CONTAINER_FWD_BAD_DEQUE
#endif

#endif

#if defined(BOOST_CONTAINER_FWD_BAD_DEQUE)
#include <deque>
#endif

#if defined(BOOST_CONTAINER_FWD_BAD_BITSET)
#include <bitset>
#endif

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable:4099) // struct/class mismatch in fwd declarations
#endif

namespace std
{
    template <class T> class allocator;
    template <class charT, class traits, class Allocator> class basic_string;
    template <class charT> struct BOOST_HASH_CHAR_TRAITS;
    template <class T> class complex;
}

// gcc 3.4 and greater
namespace std
{
#if !defined(BOOST_CONTAINER_FWD_BAD_DEQUE)
    template <class T, class Allocator> class deque;
#endif

    template <class T, class Allocator> class list;
    template <class T, class Allocator> class vector;
    template <class Key, class T, class Compare, class Allocator> class map;
    template <class Key, class T, class Compare, class Allocator>
    class multimap;
    template <class Key, class Compare, class Allocator> class set;
    template <class Key, class Compare, class Allocator> class multiset;

#if !defined(BOOST_CONTAINER_FWD_BAD_BITSET)
    template <size_t N> class bitset;
#endif
    template <class T1, class T2> struct pair;
}

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

#endif

#endif
