#ifndef BOOST_SERIALIZATION_COLLECTION_TRAITS_HPP
#define BOOST_SERIALIZATION_COLLECTION_TRAITS_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// collection_traits.hpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// This header assigns a level implemenation trait to a collection type
// for all primitives.  It is needed so that archives which are meant to be
// portable don't write class information in the archive.  Since, not all
// compiles recognize the same set of primitive types, the possibility
// exists for archives to be non-portable if class information for primitive
// types is included.  This is addressed by the following macros.
#include <boost/config.hpp>
#include <boost/mpl/integral_c.hpp>
#include <boost/mpl/integral_c_tag.hpp>

#include <boost/cstdint.hpp>
#include <boost/serialization/level.hpp>

#define BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(T, C)          \
template<>                                                          \
struct implementation_level< C < T > > {                            \
    typedef mpl::integral_c_tag tag;                                \
    typedef mpl::int_<object_serializable> type;                    \
    BOOST_STATIC_CONSTANT(int, value = object_serializable);        \
};                                                                  \
/**/

#if defined(BOOST_NO_CWCHAR) || defined(BOOST_NO_INTRINSIC_WCHAR_T)
    #define BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER_WCHAR(C)
#else
    #define BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER_WCHAR(C)   \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(wchar_t, C)        \
    /**/
#endif

// determine if its necessary to handle (u)int64_t specifically
// i.e. that its not a synonym for (unsigned) long
// if there is no 64 bit int or if its the same as a long
// we shouldn't define separate functions for int64 data types.
#if defined(BOOST_NO_INT64_T) \
    || (ULONG_MAX != 0xffffffff && ULONG_MAX == 18446744073709551615u) // 2**64 - 1
#   define BOOST_NO_INTRINSIC_INT64_T
#endif

#if !defined(BOOST_NO_INTRINSIC_INT64_T)
    #define BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER_INT64(C)    \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(boost::int64_t, C)  \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(boost::uint64_t, C) \
    /**/
#else
    #define BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER_INT64(C)
#endif

#define BOOST_SERIALIZATION_COLLECTION_TRAITS(C)                     \
    namespace boost { namespace serialization {                      \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(bool, C)            \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(char, C)            \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(signed char, C)     \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(unsigned char, C)   \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(signed int, C)      \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(unsigned int, C)    \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(signed long, C)     \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(unsigned long, C)   \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(float, C)           \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(double, C)          \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(unsigned short, C)  \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER(signed short, C)    \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER_INT64(C)            \
    BOOST_SERIALIZATION_COLLECTION_TRAITS_HELPER_WCHAR(C)            \
    } }                                                              \
    /**/

#endif // BOOST_SERIALIZATION_COLLECTION_TRAITS
