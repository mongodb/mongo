// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_SIZE_HPP
#define BOOST_RANGE_SIZE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/range/config.hpp>

#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
#include <boost/range/detail/size.hpp>
#else

#include <boost/range/detail/implementation_help.hpp>
#include <boost/range/size_type.hpp>
#include <cstddef>
#include <iterator>
#include <utility>

namespace boost 
{

#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))    
namespace range_detail 
{
#endif
        //////////////////////////////////////////////////////////////////////
        // primary template
        //////////////////////////////////////////////////////////////////////
        
        template< typename C >
        inline BOOST_DEDUCED_TYPENAME C::size_type
        boost_range_size(  const C& c )
        {
            return c.size(); 
        }

        //////////////////////////////////////////////////////////////////////
        // pair
        //////////////////////////////////////////////////////////////////////

        template< typename Iterator >
        inline std::size_t boost_range_size(  const std::pair<Iterator,Iterator>& p )
        {
            return std::distance( p.first, p.second );
        }

        //////////////////////////////////////////////////////////////////////
        // array
        //////////////////////////////////////////////////////////////////////

        template< typename T, std::size_t sz >
        inline std::size_t boost_range_size(  const T (&array)[sz] )
        {
            return range_detail::array_size<T,sz>( array ); 
        }
        
        template< typename T, std::size_t sz >
        inline std::size_t boost_range_size(  T (&array)[sz] )
        {
            return boost::range_detail::array_size<T,sz>( array );
        }
        
        //////////////////////////////////////////////////////////////////////
        // string
        //////////////////////////////////////////////////////////////////////

        inline std::size_t boost_range_size(  const char* const& s )
        {
            return boost::range_detail::str_size( s );
        }

        inline std::size_t boost_range_size(  const wchar_t* const& s )
        {
            return boost::range_detail::str_size( s );
        }

#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))                
} // namespace 'range_detail'
#endif

template< class T >
inline BOOST_DEDUCED_TYPENAME range_size<T>::type size(  const T& r )
{
#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))        
    using namespace range_detail;
#endif    
    return boost_range_size( r );
}


#if BOOST_WORKAROUND(__MWERKS__, <= 0x3003 ) || BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
// BCB and CW are not able to overload pointer when class overloads are also available.
inline range_size<const char*>::type size(  const char* r ) {
    return range_detail::str_size( r );
}
inline range_size<char*>::type size(  char* r ) {
    return range_detail::str_size( r );
}
inline range_size<const wchar_t*>::type size(  const wchar_t* r ) {
    return range_detail::str_size( r );
}
inline range_size<wchar_t*>::type size(  wchar_t* r ) {
    return range_detail::str_size( r );
}
#endif


} // namespace 'boost'

#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING

#endif
