// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_VALUE_TYPE_HPP
#define BOOST_RANGE_VALUE_TYPE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/range/config.hpp>
#include <boost/range/iterator.hpp>

#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#include <boost/range/detail/value_type.hpp>
#else

#include <boost/iterator/iterator_traits.hpp>

namespace boost
{
        template< class T >
    struct range_value
        {
                typedef BOOST_DEDUCED_TYPENAME iterator_value<
                        BOOST_DEDUCED_TYPENAME range_iterator<T>::type >::type
                                type;
        };
}

/*
#include <cstddef>
#include <utility>


namespace boost
{
    //////////////////////////////////////////////////////////////////////////
    // default
    //////////////////////////////////////////////////////////////////////////

    template< typename C >
    struct range_value
    {
        typedef BOOST_DEDUCED_TYPENAME C::value_type type;
    };

    //////////////////////////////////////////////////////////////////////////
    // pair
    //////////////////////////////////////////////////////////////////////////

    template< typename Iterator >
    struct range_value< std::pair<Iterator,Iterator> >
    {
        typedef BOOST_DEDUCED_TYPENAME
            iterator_value<Iterator>::type type;
    };


    template< typename Iterator >
    struct range_value< const std::pair<Iterator,Iterator> >
    {
        typedef BOOST_DEDUCED_TYPENAME
            iterator_value<Iterator>::type type;
    };

    //////////////////////////////////////////////////////////////////////////
    // array
    //////////////////////////////////////////////////////////////////////////

    template< typename T, std::size_t sz >
    struct range_value< T[sz] >
    {
        typedef T type;
    };

    template< typename T, std::size_t sz >
    struct range_value< const T[sz] >
    {
        typedef const T type;
    };

    //////////////////////////////////////////////////////////////////////////
    // string
    //////////////////////////////////////////////////////////////////////////

    template<>
    struct range_value< char* >
    {
        typedef char type;
    };

    template<>
    struct range_value< wchar_t* >
    {
        typedef wchar_t type;
    };

    template<>
    struct range_value< const char* >
    {
        typedef const char type;
    };

    template<>
    struct range_value< const wchar_t* >
    {
        typedef const wchar_t type;
    };

    template<>
    struct range_value< char* const >
    {
        typedef char type;
    };

    template<>
    struct range_value< wchar_t* const >
    {
        typedef wchar_t type;
    };

    template<>
    struct range_value< const char* const >
    {
        typedef const char type;
    };

    template<>
    struct range_value< const wchar_t* const >
    {
        typedef const wchar_t type;
    };

} // namespace boost
*/
#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

#endif
