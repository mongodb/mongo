// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_RESULT_ITERATOR_HPP
#define BOOST_RANGE_RESULT_ITERATOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/range/config.hpp>
#include <boost/range/iterator.hpp>
#include <boost/range/const_iterator.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/mpl/if.hpp>

namespace boost
{
    //////////////////////////////////////////////////////////////////////////
    // default
    //////////////////////////////////////////////////////////////////////////
    
    template< typename C >
    struct range_result_iterator
    {
        typedef BOOST_RANGE_DEDUCED_TYPENAME 
            mpl::if_< BOOST_DEDUCED_TYPENAME is_const<C>::type, 
                      BOOST_DEDUCED_TYPENAME range_const_iterator<C>::type,
                      BOOST_DEDUCED_TYPENAME range_iterator<C>::type >::type type;
    };
    
} // namespace boost

//#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

#endif
