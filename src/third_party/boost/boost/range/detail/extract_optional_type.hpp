// Boost.Range library
//
//  Copyright Arno Schoedl & Neil Groves 2009.
//  Use, modification and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//
#ifndef BOOST_RANGE_DETAIL_EXTRACT_OPTIONAL_TYPE_HPP_INCLUDED
#define BOOST_RANGE_DETAIL_EXTRACT_OPTIONAL_TYPE_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/config.hpp>

#ifdef BOOST_NO_PARTIAL_TEMPLATE_SPECIALIZATION

#define BOOST_RANGE_EXTRACT_OPTIONAL_TYPE( a_typedef )                         \
    template< typename C >                                                     \
    struct extract_ ## a_typedef                                               \
    {                                                                          \
        typedef BOOST_DEDUCED_TYPENAME C::a_typedef type;                      \
    };

#else

namespace boost {
    namespace range_detail {
        template< typename T > struct exists { typedef void type; };
    }
}

// Defines extract_some_typedef<T> which exposes T::some_typedef as
// extract_some_typedef<T>::type if T::some_typedef exists. Otherwise
// extract_some_typedef<T> is empty.
#define BOOST_RANGE_EXTRACT_OPTIONAL_TYPE( a_typedef )                         \
    template< typename C, typename Enable=void >                               \
    struct extract_ ## a_typedef                                               \
    {};                                                                        \
    template< typename C >                                                     \
    struct extract_ ## a_typedef< C                                            \
    , BOOST_DEDUCED_TYPENAME boost::range_detail::exists< BOOST_DEDUCED_TYPENAME C::a_typedef >::type \
    > {                                                                        \
        typedef BOOST_DEDUCED_TYPENAME C::a_typedef type;                      \
    };

#endif

#endif // include guard
