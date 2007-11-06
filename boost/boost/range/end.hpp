// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_END_HPP
#define BOOST_RANGE_END_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/type_traits/remove_const.hpp>
#include <boost/range/config.hpp>

#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
#include <boost/range/detail/end.hpp>
#else

#include <boost/range/detail/implementation_help.hpp>
#include <boost/range/iterator.hpp>
#include <boost/range/const_iterator.hpp>

namespace boost
{

#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564)) && \
    !BOOST_WORKAROUND(__GNUC__, < 3) \
    /**/
namespace range_detail
{
#endif

        //////////////////////////////////////////////////////////////////////
        // primary template
        //////////////////////////////////////////////////////////////////////

        template< typename C >
        inline BOOST_DEDUCED_TYPENAME range_const_iterator<C>::type
        boost_range_end( const C& c )
        {
            return c.end();
        }

        template< typename C >
                inline BOOST_DEDUCED_TYPENAME range_iterator<
                                        typename remove_const<C>::type >::type
        boost_range_end( C& c )
        {
            return c.end();
        }

        //////////////////////////////////////////////////////////////////////
        // pair
        //////////////////////////////////////////////////////////////////////

        template< typename Iterator >
        inline Iterator boost_range_end( const std::pair<Iterator,Iterator>& p )
        {
            return p.second;
        }

        template< typename Iterator >
        inline Iterator boost_range_end( std::pair<Iterator,Iterator>& p )
        {
            return p.second;
        }

        //////////////////////////////////////////////////////////////////////
        // array
        //////////////////////////////////////////////////////////////////////

        template< typename T, std::size_t sz >
        inline const T* boost_range_end( const T (&array)[sz] )
        {
            return range_detail::array_end<T,sz>( array );
        }

        template< typename T, std::size_t sz >
        inline T* boost_range_end( T (&array)[sz] )
        {
            return range_detail::array_end<T,sz>( array );
        }

        //////////////////////////////////////////////////////////////////////
        // string
        //////////////////////////////////////////////////////////////////////

#if 1 || BOOST_WORKAROUND(__MWERKS__, <= 0x3204 ) || BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
// CW up to 9.3 and borland have troubles with function ordering
        inline char* boost_range_end( char* s )
        {
            return range_detail::str_end( s );
        }

        inline wchar_t* boost_range_end( wchar_t* s )
        {
            return range_detail::str_end( s );
        }

        inline const char* boost_range_end( const char* s )
        {
            return range_detail::str_end( s );
        }

        inline const wchar_t* boost_range_end( const wchar_t* s )
        {
            return range_detail::str_end( s );
        }
#else
        inline char* boost_range_end( char*& s )
        {
            return range_detail::str_end( s );
        }

        inline wchar_t* boost_range_end( wchar_t*& s )
        {
            return range_detail::str_end( s );
        }

        inline const char* boost_range_end( const char*& s )
        {
            return range_detail::str_end( s );
        }

        inline const wchar_t* boost_range_end( const wchar_t*& s )
        {
            return range_detail::str_end( s );
        }
#endif

#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564)) && \
    !BOOST_WORKAROUND(__GNUC__, < 3) \
    /**/
} // namespace 'range_detail'
#endif

template< class T >
inline BOOST_DEDUCED_TYPENAME range_iterator<
                typename remove_const<T>::type >::type end( T& r )
{
#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564)) && \
    !BOOST_WORKAROUND(__GNUC__, < 3) \
    /**/
    using namespace range_detail;
#endif
    return boost_range_end( r );
}

template< class T >
inline BOOST_DEDUCED_TYPENAME range_const_iterator<T>::type end( const T& r )
{
#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564)) && \
    !BOOST_WORKAROUND(__GNUC__, < 3) \
    /**/
    using namespace range_detail;
#endif
    return boost_range_end( r );
}



#if BOOST_WORKAROUND(__MWERKS__, <= 0x3003 ) || BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
// BCB and CW are not able to overload pointer when class overloads are also available.
template<>
inline range_const_iterator<const char*>::type end<const char*>( const char*& r )
{
    return range_detail::str_end( r );
}

template<>
inline range_const_iterator<const wchar_t*>::type end<const wchar_t*>( const wchar_t*& r )
{
    return range_detail::str_end( r );
}

#endif

} // namespace 'boost'



#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING


namespace boost
{
    template< class T >
    inline BOOST_DEDUCED_TYPENAME range_const_iterator<T>::type
    const_end( const T& r )
    {
        return boost::end( r );
    }
}

#endif
