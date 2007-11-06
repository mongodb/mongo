// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_REVERSE_RESULT_ITERATOR_HPP
#define BOOST_RANGE_REVERSE_RESULT_ITERATOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/range/config.hpp>
#include <boost/range/result_iterator.hpp>
#include <boost/iterator/reverse_iterator.hpp>

namespace boost
{
    //////////////////////////////////////////////////////////////////////////
    // default
    //////////////////////////////////////////////////////////////////////////
   
    template< typename C >
    struct range_reverse_result_iterator
    {
        typedef reverse_iterator< 
            BOOST_RANGE_DEDUCED_TYPENAME range_result_iterator<C>::type > type;
    };
    
} // namespace boost

#endif
