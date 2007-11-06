// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_RBEGIN_HPP
#define BOOST_RANGE_RBEGIN_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/range/end.hpp>
#include <boost/range/reverse_result_iterator.hpp>
#include <boost/range/reverse_iterator.hpp>
#include <boost/range/const_reverse_iterator.hpp>

namespace boost
{

#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING

template< class C >
inline BOOST_DEDUCED_TYPENAME range_reverse_result_iterator<C>::type
rbegin( C& c )
{
    return BOOST_DEDUCED_TYPENAME range_reverse_result_iterator<C>::type( end( c ) );
}

#else

template< class C >
inline BOOST_DEDUCED_TYPENAME range_reverse_iterator<
                                                                typename remove_const<C>::type >::type
rbegin( C& c )
{
    typedef BOOST_DEDUCED_TYPENAME range_reverse_iterator<
                                                                typename remove_const<C>::type >::type
        iter_type;
    return iter_type( boost::end( c ) );
}

template< class C >
inline BOOST_DEDUCED_TYPENAME range_const_reverse_iterator<C>::type
rbegin( const C& c )
{
    typedef BOOST_DEDUCED_TYPENAME range_const_reverse_iterator<C>::type
        iter_type;
    return iter_type( boost::end( c ) );
}

#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING

template< class T >
inline BOOST_DEDUCED_TYPENAME range_const_reverse_iterator<T>::type
const_rbegin( const T& r )
{
    return boost::rbegin( r );
}

} // namespace 'boost'

#endif
