#ifndef BOOST_CORE_STRING_VIEW_HPP_INCLUDED
#define BOOST_CORE_STRING_VIEW_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

// boost::core::basic_string_view<Ch>
//
// Copyright 2021 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/core/enable_if.hpp>
#include <boost/core/detail/is_same.hpp>
#include <boost/assert.hpp>
#include <boost/assert/source_location.hpp>
#include <boost/throw_exception.hpp>
#include <boost/config.hpp>
#include <string>
#include <algorithm>
#include <utility>
#include <stdexcept>
#include <cstddef>
#include <cstring>
#include <climits>
#include <iosfwd>
#include <ios>
#if !defined(BOOST_NO_CXX17_HDR_STRING_VIEW)
# include <string_view>
#endif
#if !defined(BOOST_NO_CXX20_HDR_CONCEPTS) // std::common_reference_with
# include <type_traits>
#endif

namespace boost
{

// forward declaration of boost::basic_string_view from Utility
template<class Ch, class Tr> class basic_string_view;

namespace core
{
namespace detail
{

template<class Ch> struct sv_to_uchar
{
    typedef Ch type;
};

template<> struct sv_to_uchar<char>
{
    typedef unsigned char type;
};

#if defined(__GNUC__) && __GNUC__ * 100 + __GNUC_MINOR__ >= 406
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wtype-limits"
#endif

template<class Ch> BOOST_CXX14_CONSTEXPR std::size_t find_first_of( Ch const* p_, std::size_t n_, Ch const* s, std::size_t pos, std::size_t n ) BOOST_NOEXCEPT
{
    typedef typename sv_to_uchar<Ch>::type UCh;

    unsigned char table[ 256 ] = {};

    bool use_table = true;

    for( std::size_t j = 0; j < n; ++j )
    {
        UCh ch = static_cast<UCh>( s[ j ] );

        if( ch >= 0 && ch < 256 )
        {
            table[ ch ] = 1;
        }
        else
        {
            use_table = false;
            break;
        }
    }

    if( use_table )
    {
        for( std::size_t i = pos; i < n_; ++i )
        {
            UCh ch = static_cast<UCh>( p_[ i ] );
            if( ch >= 0 && ch < 256 && table[ ch ] ) return i;
        }
    }
    else if( n >= 16 )
    {
        for( std::size_t i = pos; i < n_; ++i )
        {
            Ch ch = p_[ i ];
            if( std::char_traits<Ch>::find( s, n, ch ) != 0 ) return i;
        }
    }
    else
    {
        for( std::size_t i = pos; i < n_; ++i )
        {
            Ch ch = p_[ i ];

            for( std::size_t j = 0; j < n; ++j )
            {
                if( s[ j ] == ch ) return i;
            }
        }
    }

    return static_cast<std::size_t>( -1 );
}

template<class Ch> BOOST_CXX14_CONSTEXPR std::size_t find_last_of( Ch const* p_, Ch const* s, std::size_t pos, std::size_t n ) BOOST_NOEXCEPT
{
    typedef typename sv_to_uchar<Ch>::type UCh;

    unsigned char table[ 256 ] = {};

    bool use_table = true;

    for( std::size_t j = 0; j < n; ++j )
    {
        UCh ch = static_cast<UCh>( s[ j ] );

        if( ch >= 0 && ch < 256 )
        {
            table[ ch ] = 1;
        }
        else
        {
            use_table = false;
            break;
        }
    }

    std::size_t const npos = static_cast< std::size_t >( -1 );

    std::size_t i = pos;

    if( use_table )
    {
        do
        {
            UCh ch = static_cast<UCh>( p_[ i ] );

            if( ch >= 0 && ch < 256 && table[ ch ] ) return i;

            --i;
        }
        while( i != npos );
    }
    else if( n >= 16 )
    {
        do
        {
            Ch ch = p_[ i ];

            if( std::char_traits<Ch>::find( s, n, ch ) != 0 ) return i;

            --i;
        }
        while( i != npos );
    }
    else
    {
        do
        {
            Ch ch = p_[ i ];

            for( std::size_t j = 0; j < n; ++j )
            {
                if( s[ j ] == ch ) return i;
            }

            --i;
        }
        while( i != npos );
    }

    return npos;
}

template<class Ch> BOOST_CXX14_CONSTEXPR std::size_t find_first_not_of( Ch const* p_, std::size_t n_, Ch const* s, std::size_t pos, std::size_t n ) BOOST_NOEXCEPT
{
    typedef typename sv_to_uchar<Ch>::type UCh;

    unsigned char table[ 256 ] = {};

    bool use_table = true;

    for( std::size_t j = 0; j < n; ++j )
    {
        UCh ch = static_cast<UCh>( s[ j ] );

        if( ch >= 0 && ch < 256 )
        {
            table[ ch ] = 1;
        }
        else
        {
            use_table = false;
            break;
        }
    }

    if( use_table )
    {
        for( std::size_t i = pos; i < n_; ++i )
        {
            UCh ch = static_cast<UCh>( p_[ i ] );
            if( !( ch >= 0 && ch < 256 && table[ ch ] ) ) return i;
        }
    }
    else if( n >= 16 )
    {
        for( std::size_t i = pos; i < n_; ++i )
        {
            Ch ch = p_[ i ];
            if( std::char_traits<Ch>::find( s, n, ch ) == 0 ) return i;
        }
    }
    else
    {
        for( std::size_t i = pos; i < n_; ++i )
        {
            Ch ch = p_[ i ];

            bool r = false;

            for( std::size_t j = 0; j < n; ++j )
            {
                if( s[ j ] == ch )
                {
                    r = true;
                    break;
                }
            }

            if( !r ) return i;
        }
    }

    return static_cast<std::size_t>( -1 );
}

template<class Ch> BOOST_CXX14_CONSTEXPR std::size_t find_last_not_of( Ch const* p_, Ch const* s, std::size_t pos, std::size_t n ) BOOST_NOEXCEPT
{
    typedef typename sv_to_uchar<Ch>::type UCh;

    unsigned char table[ 256 ] = {};

    bool use_table = true;

    for( std::size_t j = 0; j < n; ++j )
    {
        UCh ch = static_cast<UCh>( s[ j ] );

        if( ch >= 0 && ch < 256 )
        {
            table[ ch ] = 1;
        }
        else
        {
            use_table = false;
            break;
        }
    }

    std::size_t const npos = static_cast< std::size_t >( -1 );

    std::size_t i = pos;

    if( use_table )
    {
        do
        {
            UCh ch = static_cast<UCh>( p_[ i ] );

            if( !( ch >= 0 && ch < 256 && table[ ch ] ) ) return i;

            --i;
        }
        while( i != npos );
    }
    else if( n >= 16 )
    {
        do
        {
            Ch ch = p_[ i ];

            if( std::char_traits<Ch>::find( s, n, ch ) == 0 ) return i;

            --i;
        }
        while( i != npos );
    }
    else
    {
        do
        {
            Ch ch = p_[ i ];

            bool r = false;

            for( std::size_t j = 0; j < n; ++j )
            {
                if( s[ j ] == ch )
                {
                    r = true;
                    break;
                }
            }

            if( !r ) return i;

            --i;
        }
        while( i != npos );
    }

    return npos;
}

#if defined(__GNUC__) && __GNUC__ * 100 + __GNUC_MINOR__ >= 406
# pragma GCC diagnostic pop
#endif

} // namespace detail

template<class Ch> class basic_string_view
{
private:

    Ch const* p_;
    std::size_t n_;

public:

    // types

    typedef std::char_traits<Ch> traits_type;
    typedef Ch value_type;
    typedef Ch* pointer;
    typedef Ch const* const_pointer;
    typedef Ch& reference;
    typedef Ch const& const_reference;
    typedef Ch const* const_iterator;
    typedef const_iterator iterator;
    typedef std::reverse_iterator<const_iterator> const_reverse_iterator;
    typedef const_reverse_iterator reverse_iterator;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;

    // npos

    BOOST_STATIC_CONSTEXPR size_type npos = static_cast<size_type>( -1 );

public:

    // construction and assignment

    BOOST_CONSTEXPR basic_string_view() BOOST_NOEXCEPT: p_(), n_()
    {
    }

    BOOST_CONSTEXPR basic_string_view( Ch const* str ) BOOST_NOEXCEPT: p_( str ), n_( traits_type::length( str ) )
    {
    }

    BOOST_CONSTEXPR basic_string_view( Ch const* str, size_type len ) BOOST_NOEXCEPT: p_( str ), n_( len )
    {
    }

    template<class End> BOOST_CXX14_CONSTEXPR basic_string_view( Ch const* first, End last,
        typename boost::enable_if<boost::core::detail::is_same<End, Ch const*>, int >::type = 0 ) BOOST_NOEXCEPT: p_( first ), n_( static_cast<size_type>( last - first ) )
    {
        BOOST_ASSERT( last - first >= 0 );
    }

    template<class A> basic_string_view( std::basic_string<Ch, std::char_traits<Ch>, A> const& str ) BOOST_NOEXCEPT: p_( str.data() ), n_( str.size() )
    {
    }

#if !defined(BOOST_NO_CXX17_HDR_STRING_VIEW)

    BOOST_CONSTEXPR basic_string_view( std::basic_string_view<Ch, std::char_traits<Ch> > const& str ) BOOST_NOEXCEPT: p_( str.data() ), n_( str.size() )
    {
    }

#endif

    template<class Ch2> basic_string_view( boost::basic_string_view<Ch2, std::char_traits<Ch2> > const& str,
        typename boost::enable_if<boost::core::detail::is_same<Ch, Ch2>, int >::type = 0 ) BOOST_NOEXCEPT: p_( str.data() ), n_( str.size() )
    {
    }

#if !defined(BOOST_NO_CXX11_NULLPTR)
# if !defined(BOOST_NO_CXX11_DELETED_FUNCTIONS)

    basic_string_view( std::nullptr_t ) = delete;

# else

private:

    basic_string_view( std::nullptr_t );

public:

# endif
#endif

    // BOOST_CONSTEXPR basic_string_view& operator=( basic_string_view const& ) BOOST_NOEXCEPT & = default;

    // conversions

    template<class A> operator std::basic_string<Ch, std::char_traits<Ch>, A>() const
    {
        return std::basic_string<Ch, std::char_traits<Ch>, A>( data(), size() );
    }

#if !defined(BOOST_NO_CXX17_HDR_STRING_VIEW)

    template<class Ch2, class En = typename boost::enable_if<boost::core::detail::is_same<Ch2, Ch> >::type>
    operator std::basic_string_view<Ch2>() const BOOST_NOEXCEPT
    {
        return std::basic_string_view<Ch>( data(), size() );
    }

#endif

    template<class Ch2> operator boost::basic_string_view<Ch2,
        typename boost::enable_if<boost::core::detail::is_same<Ch2, Ch>, std::char_traits<Ch> >::type> () const BOOST_NOEXCEPT
    {
        return boost::basic_string_view< Ch, std::char_traits<Ch> >( data(), size() );
    }

    // iterator support

    BOOST_CONSTEXPR const_iterator begin() const BOOST_NOEXCEPT
    {
        return p_;
    }

    BOOST_CONSTEXPR const_iterator end() const BOOST_NOEXCEPT
    {
        return p_ + n_;
    }

    BOOST_CONSTEXPR const_iterator cbegin() const BOOST_NOEXCEPT
    {
        return p_;
    }

    BOOST_CONSTEXPR const_iterator cend() const BOOST_NOEXCEPT
    {
        return p_ + n_;
    }

    BOOST_CONSTEXPR const_reverse_iterator rbegin() const BOOST_NOEXCEPT
    {
        return const_reverse_iterator( end() );
    }

    BOOST_CONSTEXPR const_reverse_iterator rend() const BOOST_NOEXCEPT
    {
        return const_reverse_iterator( begin() );
    }

    BOOST_CONSTEXPR const_reverse_iterator crbegin() const BOOST_NOEXCEPT
    {
        return const_reverse_iterator( end() );
    }

    BOOST_CONSTEXPR const_reverse_iterator crend() const BOOST_NOEXCEPT
    {
        return const_reverse_iterator( begin() );
    }

    // capacity

    BOOST_CONSTEXPR size_type size() const BOOST_NOEXCEPT
    {
        return n_;
    }

    BOOST_CONSTEXPR size_type length() const BOOST_NOEXCEPT
    {
        return n_;
    }

    BOOST_CONSTEXPR size_type max_size() const BOOST_NOEXCEPT
    {
        return npos / sizeof( Ch );
    }

    BOOST_CONSTEXPR bool empty() const BOOST_NOEXCEPT
    {
        return n_ == 0;
    }

    // element access

    BOOST_CXX14_CONSTEXPR const_reference operator[]( size_type pos ) const BOOST_NOEXCEPT
    {
        BOOST_ASSERT( pos < size() );
        return p_[ pos ];
    }

    BOOST_CXX14_CONSTEXPR const_reference at( size_type pos ) const
    {
        if( pos >= size() )
        {
            boost::throw_exception( std::out_of_range( "basic_string_view::at" ), BOOST_CURRENT_LOCATION );
        }

        return p_[ pos ];
    }

    BOOST_CXX14_CONSTEXPR const_reference front() const BOOST_NOEXCEPT
    {
        BOOST_ASSERT( !empty() );
        return p_[ 0 ];
    }

    BOOST_CXX14_CONSTEXPR const_reference back() const BOOST_NOEXCEPT
    {
        BOOST_ASSERT( !empty() );
        return p_[ n_ - 1 ];
    }

    BOOST_CONSTEXPR const_pointer data() const BOOST_NOEXCEPT
    {
        return p_;
    }

    // modifiers

    BOOST_CXX14_CONSTEXPR void remove_prefix( size_type n ) BOOST_NOEXCEPT
    {
        BOOST_ASSERT( n <= size() );

        p_ += n;
        n_ -= n;
    }

    BOOST_CXX14_CONSTEXPR void remove_suffix( size_type n ) BOOST_NOEXCEPT
    {
        BOOST_ASSERT( n <= size() );

        n_ -= n;
    }

    BOOST_CXX14_CONSTEXPR void swap( basic_string_view& s ) BOOST_NOEXCEPT
    {
        std::swap( p_, s.p_ );
        std::swap( n_, s.n_ );
    }

    // string operations

    BOOST_CXX14_CONSTEXPR size_type copy( Ch* s, size_type n, size_type pos = 0 ) const
    {
        if( pos > size() )
        {
            boost::throw_exception( std::out_of_range( "basic_string_view::copy" ), BOOST_CURRENT_LOCATION );
        }

        std::size_t rlen = (std::min)( n, size() - pos );

        traits_type::copy( s, data() + pos, rlen );

        return rlen;
    }

    BOOST_CXX14_CONSTEXPR basic_string_view substr( size_type pos = 0, size_type n = npos ) const
    {
        if( pos > size() )
        {
            boost::throw_exception( std::out_of_range( "basic_string_view::substr" ), BOOST_CURRENT_LOCATION );
        }

        std::size_t rlen = (std::min)( n, size() - pos );

        return basic_string_view( data() + pos, rlen );
    }

    // compare

    BOOST_CXX14_CONSTEXPR int compare( basic_string_view str ) const BOOST_NOEXCEPT
    {
        std::size_t rlen = (std::min)( size(), str.size() );

        int cmp = traits_type::compare( data(), str.data(), rlen );

        if( cmp != 0 ) return cmp;

        if( size() == str.size() ) return 0;

        return size() < str.size()? -1: +1;
    }

    BOOST_CONSTEXPR int compare( size_type pos1, size_type n1, basic_string_view str ) const
    {
        return substr( pos1, n1 ).compare( str );
    }

    BOOST_CONSTEXPR int compare( size_type pos1, size_type n1, basic_string_view str, size_type pos2, size_type n2 ) const
    {
        return substr( pos1, n1 ).compare( str.substr( pos2, n2 ) );
    }

    BOOST_CONSTEXPR int compare( Ch const* s ) const BOOST_NOEXCEPT
    {
        return compare( basic_string_view( s ) );
    }

    BOOST_CONSTEXPR int compare( size_type pos1, size_type n1, Ch const* s ) const
    {
        return substr( pos1, n1 ).compare( basic_string_view( s ) );
    }

    BOOST_CONSTEXPR int compare( size_type pos1, size_type n1, Ch const* s, size_type n2 ) const
    {
        return substr( pos1, n1 ).compare( basic_string_view( s, n2 ) );
    }

    // starts_with

    BOOST_CONSTEXPR bool starts_with( basic_string_view x ) const BOOST_NOEXCEPT
    {
        return size() >= x.size() && traits_type::compare( data(), x.data(), x.size() ) == 0;
    }

    BOOST_CONSTEXPR bool starts_with( Ch x ) const BOOST_NOEXCEPT
    {
        return !empty() && front() == x;
    }

    BOOST_CONSTEXPR bool starts_with( Ch const* x ) const BOOST_NOEXCEPT
    {
        return starts_with( basic_string_view( x ) );
    }

    // ends_with

    BOOST_CONSTEXPR bool ends_with( basic_string_view x ) const BOOST_NOEXCEPT
    {
        return size() >= x.size() && traits_type::compare( data() + size() - x.size(), x.data(), x.size() ) == 0;
    }

    BOOST_CONSTEXPR bool ends_with( Ch x ) const BOOST_NOEXCEPT
    {
        return !empty() && back() == x;
    }

    BOOST_CONSTEXPR bool ends_with( Ch const* x ) const BOOST_NOEXCEPT
    {
        return ends_with( basic_string_view( x ) );
    }

    // find

    BOOST_CONSTEXPR size_type find( basic_string_view str, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        return find( str.data(), pos, str.size() );
    }

    BOOST_CXX14_CONSTEXPR size_type find( Ch c, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        if( pos >= size() ) return npos;

        Ch const* r = traits_type::find( data() + pos, size() - pos, c );

        return r? static_cast<size_type>( r - data() ): npos;
    }

    BOOST_CXX14_CONSTEXPR size_type find( Ch const* s, size_type pos, size_type n ) const BOOST_NOEXCEPT
    {
        if( n == 1 ) return find( s[0], pos );

        if( pos + n > size() ) return npos;
        if( n == 0 ) return pos;

        Ch const* p = data() + pos;
        Ch const* last = data() + size() - n + 1;

        for( ;; )
        {
            p = traits_type::find( p, static_cast<size_type>( last - p ), s[0] );

            if( p == 0 ) break;

            if( traits_type::compare( p + 1, s + 1, n - 1 ) == 0 ) return static_cast<size_type>( p - data() );

            ++p;
        }

        return npos;
    }

    BOOST_CONSTEXPR size_type find( Ch const* s, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        return find( s, pos, traits_type::length( s ) );
    }

    // rfind

    BOOST_CONSTEXPR size_type rfind( basic_string_view str, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        return rfind( str.data(), pos, str.size() );
    }

    BOOST_CXX14_CONSTEXPR size_type rfind( Ch c, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        size_type n = size();

        if( n == 0 )
        {
            return npos;
        }

        if( pos > n - 1 )
        {
            pos = n - 1;
        }

        do
        {
            if( p_[ pos ] == c ) return pos;
            --pos;
        }
        while( pos != npos );

        return npos;
    }

    BOOST_CXX14_CONSTEXPR size_type rfind( Ch const* s, size_type pos, size_type n ) const BOOST_NOEXCEPT
    {
        if( n > size() ) return npos;

        if( pos > size() - n )
        {
            pos = size() - n;
        }

        if( n == 0 ) return pos;

        for( ;; )
        {
            size_type xpos = rfind( s[0], pos );

            if( xpos == npos ) return npos;

            if( traits_type::compare( data() + xpos, s, n ) == 0 ) return xpos;

            if( xpos == 0 ) return npos;

            pos = xpos - 1;
        }
    }

    BOOST_CONSTEXPR size_type rfind( Ch const* s, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        return rfind( s, pos, traits_type::length( s ) );
    }

    // find_first_of

    BOOST_CXX14_CONSTEXPR size_type find_first_of( basic_string_view str, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        return find_first_of( str.data(), pos, str.size() );
    }

    BOOST_CONSTEXPR size_type find_first_of( Ch c, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        return find( c, pos );
    }

    BOOST_CXX14_CONSTEXPR size_type find_first_of( Ch const* s, size_type pos, size_type n ) const BOOST_NOEXCEPT
    {
        if( n == 0 || pos >= size() ) return npos;
        if( n == 1 ) return find( s[0], pos );

        return detail::find_first_of( data(), size(), s, pos, n );
    }

    BOOST_CXX14_CONSTEXPR size_type find_first_of( Ch const* s, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        return find_first_of( s, pos, traits_type::length( s ) );
    }

    // find_last_of

    BOOST_CXX14_CONSTEXPR size_type find_last_of( basic_string_view str, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        return find_last_of( str.data(), pos, str.size() );
    }

    BOOST_CONSTEXPR size_type find_last_of( Ch c, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        return rfind( c, pos );
    }

    BOOST_CXX14_CONSTEXPR size_type find_last_of( Ch const* s, size_type pos, size_type n ) const BOOST_NOEXCEPT
    {
        if( n == 1 )
        {
            return rfind( s[0], pos );
        }

        size_type m = size();

        if( m == 0 )
        {
            return npos;
        }

        if( pos > m - 1 )
        {
            pos = m - 1;
        }

        return detail::find_last_of( data(), s, pos, n );
    }

    BOOST_CXX14_CONSTEXPR size_type find_last_of( Ch const* s, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        return find_last_of( s, pos, traits_type::length( s ) );
    }

    // find_first_not_of

    BOOST_CXX14_CONSTEXPR size_type find_first_not_of( basic_string_view str, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        return find_first_not_of( str.data(), pos, str.size() );
    }

    BOOST_CXX14_CONSTEXPR size_type find_first_not_of( Ch c, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        for( std::size_t i = pos; i < n_; ++i )
        {
            if( p_[ i ] != c ) return i;
        }

        return npos;
    }

    BOOST_CXX14_CONSTEXPR size_type find_first_not_of( Ch const* s, size_type pos, size_type n ) const BOOST_NOEXCEPT
    {
        if( pos >= size() ) return npos;
        if( n == 1 ) return find_first_not_of( s[0], pos );

        return detail::find_first_not_of( data(), size(), s, pos, n );
    }

    BOOST_CXX14_CONSTEXPR size_type find_first_not_of( Ch const* s, size_type pos = 0 ) const BOOST_NOEXCEPT
    {
        return find_first_not_of( s, pos, traits_type::length( s ) );
    }

    // find_last_not_of

    BOOST_CXX14_CONSTEXPR size_type find_last_not_of( basic_string_view str, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        return find_last_not_of( str.data(), pos, str.size() );
    }

    BOOST_CXX14_CONSTEXPR size_type find_last_not_of( Ch c, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        size_type m = size();

        if( m == 0 )
        {
            return npos;
        }

        if( pos > m - 1 )
        {
            pos = m - 1;
        }

        do
        {
            if( p_[ pos ] != c ) return pos;
            --pos;
        }
        while( pos != npos );

        return npos;
    }

    BOOST_CXX14_CONSTEXPR size_type find_last_not_of( Ch const* s, size_type pos, size_type n ) const BOOST_NOEXCEPT
    {
        if( n == 1 )
        {
            return find_last_not_of( s[0], pos );
        }

        size_type m = size();

        if( m == 0 )
        {
            return npos;
        }

        if( pos > m - 1 )
        {
            pos = m - 1;
        }

        return detail::find_last_not_of( data(), s, pos, n );
    }

    BOOST_CXX14_CONSTEXPR size_type find_last_not_of( Ch const* s, size_type pos = npos ) const BOOST_NOEXCEPT
    {
        return find_last_not_of( s, pos, traits_type::length( s ) );
    }

    // contains

    BOOST_CONSTEXPR bool contains( basic_string_view sv ) const BOOST_NOEXCEPT
    {
        return find( sv ) != npos;
    }

    BOOST_CXX14_CONSTEXPR bool contains( Ch c ) const BOOST_NOEXCEPT
    {
        Ch const* p = data();
        size_type n = size();

        if( n >= 16 )
        {
            return traits_type::find( p, n, c ) != 0;
        }
        else
        {
            for( size_type i = 0; i < n; ++i )
            {
                if( p[ i ] == c ) return true;
            }

            return false;
        }
    }

    BOOST_CONSTEXPR bool contains( Ch const* s ) const BOOST_NOEXCEPT
    {
        return find( s ) != npos;
    }

    // relational operators

    BOOST_CXX14_CONSTEXPR friend bool operator==( basic_string_view sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.size() == sv2.size() && traits_type::compare( sv1.data(), sv2.data(), sv1.size() ) == 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator!=( basic_string_view sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return !( sv1 == sv2 );
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<( basic_string_view sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) < 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<=( basic_string_view sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) <= 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>( basic_string_view sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) > 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>=( basic_string_view sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) >= 0;
    }

#if !defined(BOOST_NO_CXX17_HDR_STRING_VIEW)

    // "sufficient number of additional overloads"

    // against std::string_view

    BOOST_CXX14_CONSTEXPR friend bool operator==( basic_string_view sv1, std::basic_string_view<Ch> sv2 ) BOOST_NOEXCEPT
    {
        return sv1.size() == sv2.size() && traits_type::compare( sv1.data(), sv2.data(), sv1.size() ) == 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator==( std::basic_string_view<Ch> sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.size() == sv2.size() && traits_type::compare( sv1.data(), sv2.data(), sv1.size() ) == 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator!=( basic_string_view sv1, std::basic_string_view<Ch> sv2 ) BOOST_NOEXCEPT
    {
        return !( sv1 == sv2 );
    }

    BOOST_CXX14_CONSTEXPR friend bool operator!=( std::basic_string_view<Ch> sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return !( sv1 == sv2 );
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<( basic_string_view sv1, std::basic_string_view<Ch> sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) < 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<( std::basic_string_view<Ch> sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) < 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<=( basic_string_view sv1, std::basic_string_view<Ch> sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) <= 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<=( std::basic_string_view<Ch> sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) <= 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>( basic_string_view sv1, std::basic_string_view<Ch> sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) > 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>( std::basic_string_view<Ch> sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) > 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>=( basic_string_view sv1, std::basic_string_view<Ch> sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) >= 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>=( std::basic_string_view<Ch> sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) >= 0;
    }

    // against Ch const*

    BOOST_CXX14_CONSTEXPR friend bool operator==( basic_string_view sv1, Ch const* sv2 ) BOOST_NOEXCEPT
    {
        return sv1 == basic_string_view( sv2 );
    }

    BOOST_CXX14_CONSTEXPR friend bool operator==( Ch const* sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return basic_string_view( sv1 ) == sv2;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator!=( basic_string_view sv1, Ch const* sv2 ) BOOST_NOEXCEPT
    {
        return !( sv1 == sv2 );
    }

    BOOST_CXX14_CONSTEXPR friend bool operator!=( Ch const* sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return !( sv1 == sv2 );
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<( basic_string_view sv1, Ch const* sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) < 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<( Ch const* sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv2.compare( sv1 ) > 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<=( basic_string_view sv1, Ch const* sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) <= 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator<=( Ch const* sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv2.compare( sv1 ) >= 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>( basic_string_view sv1, Ch const* sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) > 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>( Ch const* sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv2.compare( sv1 ) < 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>=( basic_string_view sv1, Ch const* sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) >= 0;
    }

    BOOST_CXX14_CONSTEXPR friend bool operator>=( Ch const* sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv2.compare( sv1 ) <= 0;
    }

    // against std::string

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator==( basic_string_view sv1, std::basic_string<Ch, std::char_traits<Ch>, A> const& sv2 ) BOOST_NOEXCEPT
    {
        return sv1.size() == sv2.size() && traits_type::compare( sv1.data(), sv2.data(), sv1.size() ) == 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator==( std::basic_string<Ch, std::char_traits<Ch>, A> const& sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv1.size() == sv2.size() && traits_type::compare( sv1.data(), sv2.data(), sv1.size() ) == 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator!=( basic_string_view sv1, std::basic_string<Ch, std::char_traits<Ch>, A> const& sv2 ) BOOST_NOEXCEPT
    {
        return !( sv1 == sv2 );
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator!=( std::basic_string<Ch, std::char_traits<Ch>, A> const& sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return !( sv1 == sv2 );
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator<( basic_string_view sv1, std::basic_string<Ch, std::char_traits<Ch>, A> const& sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) < 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator<( std::basic_string<Ch, std::char_traits<Ch>, A> const& sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv2.compare( sv1 ) > 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator<=( basic_string_view sv1, std::basic_string<Ch, std::char_traits<Ch>, A> const& sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) <= 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator<=( std::basic_string<Ch, std::char_traits<Ch>, A> const& sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv2.compare( sv1 ) >= 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator>( basic_string_view sv1, std::basic_string<Ch, std::char_traits<Ch>, A> const& sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) > 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator>( std::basic_string<Ch, std::char_traits<Ch>, A> const& sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv2.compare( sv1 ) < 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator>=( basic_string_view sv1, std::basic_string<Ch, std::char_traits<Ch>, A> const& sv2 ) BOOST_NOEXCEPT
    {
        return sv1.compare( sv2 ) >= 0;
    }

    template<class A> BOOST_CXX14_CONSTEXPR friend bool operator>=( std::basic_string<Ch, std::char_traits<Ch>, A> const& sv1, basic_string_view sv2 ) BOOST_NOEXCEPT
    {
        return sv2.compare( sv1 ) <= 0;
    }

#endif
};

// stream inserter

template<class Ch> std::basic_ostream<Ch>& operator<<( std::basic_ostream<Ch>& os, basic_string_view<Ch> str )
{
    Ch const* p = str.data();
    std::streamsize n = static_cast<std::streamsize>( str.size() );

    std::streamsize m = os.width();

    if( n >= m )
    {
        os.write( p, n );
    }
    else if( ( os.flags() & std::ios_base::adjustfield ) == std::ios_base::left )
    {
        os.write( p, n );

        os.width( m - n );
        os << "";
    }
    else
    {
        os.width( m - n );
        os << "";

        os.write( p, n );
    }

    os.width( 0 );
    return os;
}

#if defined(BOOST_NO_CXX17_INLINE_VARIABLES)
template<class Ch> BOOST_CONSTEXPR_OR_CONST std::size_t basic_string_view<Ch>::npos;
#endif

// typedef names

typedef basic_string_view<char> string_view;
typedef basic_string_view<wchar_t> wstring_view;

#if !defined(BOOST_NO_CXX11_CHAR16_T)
typedef basic_string_view<char16_t> u16string_view;
#endif

#if !defined(BOOST_NO_CXX11_CHAR32_T)
typedef basic_string_view<char32_t> u32string_view;
#endif

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811L
typedef basic_string_view<char8_t> u8string_view;
#endif

} // namespace core
} // namespace boost

// std::common_reference support
// needed for iterators that have reference=string_view and value_type=std::string

#if !defined(BOOST_NO_CXX20_HDR_CONCEPTS)

template<class Ch, class A, template<class> class Q1, template<class> class Q2>
struct std::basic_common_reference<
    boost::core::basic_string_view<Ch>,
    std::basic_string<Ch, std::char_traits<Ch>, A>,
    Q1, Q2>
{
    using type = boost::core::basic_string_view<Ch>;
};

template<class Ch, class A, template<class> class Q1, template<class> class Q2>
struct std::basic_common_reference<
    std::basic_string<Ch, std::char_traits<Ch>, A>,
    boost::core::basic_string_view<Ch>,
    Q1, Q2>
{
    using type = boost::core::basic_string_view<Ch>;
};

#endif

#endif  // #ifndef BOOST_CORE_STRING_VIEW_HPP_INCLUDED
