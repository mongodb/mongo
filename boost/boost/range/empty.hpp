// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_EMPTY_HPP
#define BOOST_RANGE_EMPTY_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/range/config.hpp>
//#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
//#include <boost/range/detail/empty.hpp>
//#else

#include <boost/range/begin.hpp>
#include <boost/range/end.hpp>

namespace boost 
{ 
namespace range_detail 
{

        //////////////////////////////////////////////////////////////////////
        // primary template
        //////////////////////////////////////////////////////////////////////

        template< typename C >
        inline bool empty( const C& c )
        {
            return boost::begin( c ) == boost::end( c );
        }

        //////////////////////////////////////////////////////////////////////
        // string
        //////////////////////////////////////////////////////////////////////

        inline bool empty( const char* const& s )
        {
            return s == 0 || s[0] == 0;
        }

        inline bool empty( const wchar_t* const& s )
        {
            return s == 0 || s[0] == 0;
        }
        
} // namespace 'range_detail'

template< class T >
inline bool empty( const T& r )
{
    return range_detail::empty( r );
}

} // namepace 'boost'

//#endif //  BOOST_NO_FUNCTION_TEMPLATE_ORDERING

#endif
