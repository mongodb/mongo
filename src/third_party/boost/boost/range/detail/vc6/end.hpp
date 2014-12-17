// Boost.Range library
//
//  Copyright Thorsten Ottosen 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_DETAIL_VC6_END_HPP
#define BOOST_RANGE_DETAIL_VC6_END_HPP

#include <boost/range/detail/implementation_help.hpp>
#include <boost/range/detail/implementation_help.hpp>
#include <boost/range/result_iterator.hpp>
#include <boost/range/detail/common.hpp>
#include <boost/range/detail/remove_extent.hpp>

namespace boost 
{
    namespace range_detail
    {
        template< typename T >
        struct range_end;

        //////////////////////////////////////////////////////////////////////
        // default
        //////////////////////////////////////////////////////////////////////
        
        template<>
        struct range_end<std_container_>
        {
            template< typename C >
            struct inner {
                static BOOST_RANGE_DEDUCED_TYPENAME range_result_iterator<C>::type 
                fun( C& c )
                {
                    return c.end();
                };
            };
        };
                    
        //////////////////////////////////////////////////////////////////////
        // pair
        //////////////////////////////////////////////////////////////////////
        
        template<>
        struct range_end<std_pair_>
        {
            template< typename P >
            struct inner {
                static BOOST_RANGE_DEDUCED_TYPENAME range_result_iterator<P>::type 
                fun( const P& p )
                {
                    return p.second;
                }
            };
        };
 
        //////////////////////////////////////////////////////////////////////
        // array
        //////////////////////////////////////////////////////////////////////
        
        template<>
        struct range_end<array_>  
        {
            template< typename T >
            struct inner {
                static BOOST_DEDUCED_TYPENAME remove_extent<T>::type*
                fun(T& t)
                {
                    return t + remove_extent<T>::size;
                }
            };
        };

                
        template<>
        struct range_end<char_array_>
        {
            template< typename T >
            struct inner {
                static BOOST_DEDUCED_TYPENAME remove_extent<T>::type*
                fun(T& t)
                {
                    return t + remove_extent<T>::size;
                }
            };
        };
        
        template<>
        struct range_end<wchar_t_array_>
        {
            template< typename T >
            struct inner {
                static BOOST_DEDUCED_TYPENAME remove_extent<T>::type*
                fun(T& t)
                {
                    return t + remove_extent<T>::size;
                }
            };
        };

        //////////////////////////////////////////////////////////////////////
        // string
        //////////////////////////////////////////////////////////////////////
        
        template<>
        struct range_end<char_ptr_>
        {
            template< typename T >
            struct inner {
                static char* fun( char* s )
                {
                    return boost::range_detail::str_end( s );
                }
            };
        };

        template<>
        struct range_end<const_char_ptr_>
        {
            template< typename T >
            struct inner {
                static const char* fun( const char* s )
                {
                    return boost::range_detail::str_end( s );
                }
            };
        };

        template<>
        struct range_end<wchar_t_ptr_>
        {
            template< typename T >
            struct inner {
                static wchar_t* fun( wchar_t* s )
                {
                    return boost::range_detail::str_end( s );
                }
            };
        };


        template<>
        struct range_end<const_wchar_t_ptr_>
        {
            template< typename T >
            struct inner {
                static const wchar_t* fun( const wchar_t* s )
                {
                    return boost::range_detail::str_end( s );
                }
            };
        };
        
    } // namespace 'range_detail'
    
    template< typename C >
    inline BOOST_DEDUCED_TYPENAME range_result_iterator<C>::type 
    end( C& c )
    {
        return range_detail::range_end<range_detail::range<C>::type>::inner<C>::fun( c );
    }
    
} // namespace 'boost'


#endif
