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

#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#include <boost/range/detail/size_type.hpp>
#else

#include <boost/type_traits/remove_const.hpp>
#include <cstddef>
#include <utility>

namespace boost
{
    namespace detail
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
    
        //////////////////////////////////////////////////////////////////////////
        // array
        //////////////////////////////////////////////////////////////////////////
    
        template< typename T, std::size_t sz >
        struct range_size< T[sz] >
        {
            typedef std::size_t type;
        };
    }

    template< class T >
    struct range_size : 
        detail::range_size<T>
    { };

    template< class T >
    struct range_size<const T >
        : detail::range_size<T>
    { };
    
} // namespace boost

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION


#endif
