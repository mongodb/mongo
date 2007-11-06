// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_DETAIL_BEGIN_HPP
#define BOOST_RANGE_DETAIL_BEGIN_HPP

#include <boost/config.hpp> // BOOST_MSVC
#include <boost/detail/workaround.hpp>
#include <boost/range/result_iterator.hpp>
#include <boost/range/detail/common.hpp>
#if BOOST_WORKAROUND(BOOST_MSVC, < 1310)
# include <boost/range/value_type.hpp>
#endif

namespace boost 
{
    
    namespace range_detail
    {
        template< typename T >
        struct range_begin;

        //////////////////////////////////////////////////////////////////////
        // default
        //////////////////////////////////////////////////////////////////////
        
        template<>
        struct range_begin<std_container_>
        {
            template< typename C >
            static BOOST_RANGE_DEDUCED_TYPENAME range_result_iterator<C>::type fun( C& c )
            {
                return c.begin();
            };
        };
                    
        //////////////////////////////////////////////////////////////////////
        // pair
        //////////////////////////////////////////////////////////////////////
        
        template<>
        struct range_begin<std_pair_>
        {
            template< typename P >
            static BOOST_RANGE_DEDUCED_TYPENAME range_result_iterator<P>::type fun( const P& p )
            {
                return p.first;
            }
        };
 
        //////////////////////////////////////////////////////////////////////
        // array
        //////////////////////////////////////////////////////////////////////
        
        template<>
        struct range_begin<array_>
        {
        #if !BOOST_WORKAROUND(BOOST_MSVC, < 1310)
            template< typename T, std::size_t sz >
            static T* fun( T BOOST_RANGE_ARRAY_REF()[sz] )
            {
                return boost_range_array;
            }
        #else
            template<typename T>
            static BOOST_RANGE_DEDUCED_TYPENAME range_value<T>::type* fun(T& t)
            {
                return t;
            }
        #endif
        };

        //////////////////////////////////////////////////////////////////////
        // string
        //////////////////////////////////////////////////////////////////////
     
        template<>
        struct range_begin<char_ptr_>
        {
            static char* fun( char* s )
            {
                return s;
            }
        };

        template<>
        struct range_begin<const_char_ptr_>
        {
            static const char* fun( const char* s )
            {
                return s;
            }
        };
        
        template<>
        struct range_begin<wchar_t_ptr_>
        {
            
            static wchar_t* fun( wchar_t* s )
            {
                return s;
            }
        };

        template<>
        struct range_begin<const_wchar_t_ptr_>
        {
            static const wchar_t* fun( const wchar_t* s )
            {
                return s;
            }
        };

    } // namespace 'range_detail'
    
    template< typename C >
    inline BOOST_RANGE_DEDUCED_TYPENAME range_result_iterator<C>::type 
    begin( C& c )
    {
        return range_detail::range_begin< BOOST_RANGE_DEDUCED_TYPENAME range_detail::range<C>::type >::fun( c );
    }
    
} // namespace 'boost'


#endif
