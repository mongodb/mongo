// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_SIZE_TYPE_HPP
#define BOOST_RANGE_SIZE_TYPE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/range/config.hpp>
/*
#include <boost/range/difference_type.hpp>

namespace boost
{
        namespace range_detail
        {
                template< class T >
                struct add_unsigned;

                template<>
                struct add_unsigned<short>
                {
                        typedef unsigned short type;
                };

                template<>
                struct add_unsigned<int>
                {
                        typedef unsigned int type;
                };

                template<>
                struct add_unsigned<long>
                {
                        typedef unsigned long type;
                };

#ifdef BOOST_HAS_LONG_LONG

                template<>
                struct add_unsigned<long long>
                {
                        typedef unsigned long long type;
                };
#endif

        }

        template< class T >
        struct range_size
        {
                typedef BOOST_DEDUCED_TYPENAME range_detail::add_unsigned<
                                        BOOST_DEDUCED_TYPENAME range_difference<T>::type >::type
                        type;
        };
}
*/

#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#include <boost/range/detail/size_type.hpp>
#else

#include <cstddef>
#include <utility>

namespace boost
{
    //////////////////////////////////////////////////////////////////////////
    // default
    //////////////////////////////////////////////////////////////////////////

    template< typename C >
    struct range_size
    {
        typedef BOOST_DEDUCED_TYPENAME C::size_type type;
    };

    //////////////////////////////////////////////////////////////////////////
    // pair
    //////////////////////////////////////////////////////////////////////////

    template< typename Iterator >
    struct range_size< std::pair<Iterator,Iterator> >
    {
        typedef std::size_t type;
    };

    template< typename Iterator >
    struct range_size< const std::pair<Iterator,Iterator> >
    {
        typedef std::size_t type;
    };

    //////////////////////////////////////////////////////////////////////////
    // array
    //////////////////////////////////////////////////////////////////////////

    template< typename T, std::size_t sz >
    struct range_size< T[sz] >
    {
        typedef std::size_t type;
    };

    template< typename T, std::size_t sz >
    struct range_size< const T[sz] >
    {
        typedef std::size_t type;
    };

    //////////////////////////////////////////////////////////////////////////
    // string
    //////////////////////////////////////////////////////////////////////////

    template<>
    struct range_size< char* >
    {
        typedef std::size_t type;
    };

    template<>
    struct range_size< wchar_t* >
    {
        typedef std::size_t type;
    };

    template<>
    struct range_size< const char* >
    {
        typedef std::size_t type;
    };

    template<>
    struct range_size< const wchar_t* >
    {
        typedef std::size_t type;
    };

    template<>
    struct range_size< char* const >
    {
        typedef std::size_t type;
    };

    template<>
    struct range_size< wchar_t* const >
    {
        typedef std::size_t type;
    };

    template<>
    struct range_size< const char* const >
    {
        typedef std::size_t type;
    };

    template<>
    struct range_size< const wchar_t* const >
    {
        typedef std::size_t type;
    };

} // namespace boost

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION


#endif
