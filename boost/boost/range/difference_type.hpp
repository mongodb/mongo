// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_DIFFERENCE_TYPE_HPP
#define BOOST_RANGE_DIFFERENCE_TYPE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/range/config.hpp>
#include <boost/range/const_iterator.hpp>
#include <boost/iterator/iterator_traits.hpp>

namespace boost
{
        template< class T >
        struct range_difference
        {
                typedef BOOST_DEDUCED_TYPENAME iterator_difference<
                        BOOST_DEDUCED_TYPENAME range_const_iterator<T>::type >::type
                                type;
        };
}

//#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
//#include <boost/range/detail/difference_type.hpp>
//#else

/*
#include <cstddef>
#include <utility>

namespace boost
{
    //////////////////////////////////////////////////////////////////////////
    // default
    //////////////////////////////////////////////////////////////////////////

    template< typename C >
    struct range_difference
    {
        typedef BOOST_DEDUCED_TYPENAME C::difference_type type;
    };

    //////////////////////////////////////////////////////////////////////////
    // pair
    //////////////////////////////////////////////////////////////////////////

    template< typename Iterator >
    struct range_difference< std::pair<Iterator,Iterator> >
    {
        typedef BOOST_DEDUCED_TYPENAME
            iterator_difference<Iterator>::type type;
    };

    template< typename Iterator >
    struct range_difference< const std::pair<Iterator,Iterator> >
    {
        typedef BOOST_DEDUCED_TYPENAME
            iterator_difference<Iterator>::type type;
    };


    //////////////////////////////////////////////////////////////////////////
    // array
    //////////////////////////////////////////////////////////////////////////

    template< typename T, std::size_t sz >
    struct range_difference< T[sz] >
    {
        typedef std::ptrdiff_t type;
    };

    template< typename T, std::size_t sz >
    struct range_difference< const T[sz] >
    {
        typedef std::ptrdiff_t type;
    };

    //////////////////////////////////////////////////////////////////////////
    // string
    //////////////////////////////////////////////////////////////////////////

    template<>
    struct range_difference< char* >
    {
        typedef std::ptrdiff_t type;
    };

    template<>
    struct range_difference< wchar_t* >
    {
        typedef std::ptrdiff_t type;
    };

    template<>
    struct range_difference< const char* >
    {
        typedef std::ptrdiff_t type;
    };

    template<>
    struct range_difference< const wchar_t* >
    {
        typedef std::ptrdiff_t type;
    };

    template<>
    struct range_difference< char* const >
    {
        typedef std::ptrdiff_t type;
    };

    template<>
    struct range_difference< wchar_t* const >
    {
        typedef std::ptrdiff_t type;
    };

    template<>
    struct range_difference< const char* const >
    {
        typedef std::ptrdiff_t type;
    };

    template<>
    struct range_difference< const wchar_t* const >
    {
        typedef std::ptrdiff_t type;
    };

} // namespace boost

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
*/

#endif
