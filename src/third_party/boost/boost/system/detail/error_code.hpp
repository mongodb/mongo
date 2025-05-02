#ifndef BOOST_SYSTEM_DETAIL_ERROR_CODE_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_ERROR_CODE_HPP_INCLUDED

//  Copyright Beman Dawes 2006, 2007
//  Copyright Christoper Kohlhoff 2007
//  Copyright Peter Dimov 2017-2021
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  See library home page at http://www.boost.org/libs/system

#include <boost/system/is_error_code_enum.hpp>
#include <boost/system/detail/error_category.hpp>
#include <boost/system/detail/error_condition.hpp>
#include <boost/system/detail/system_category.hpp>
#include <boost/system/detail/system_category_impl.hpp>
#include <boost/system/detail/interop_category.hpp>
#include <boost/system/detail/enable_if.hpp>
#include <boost/system/detail/is_same.hpp>
#include <boost/system/detail/append_int.hpp>
#include <boost/system/detail/snprintf.hpp>
#include <boost/system/detail/config.hpp>
#include <boost/system/detail/std_category.hpp>
#include <boost/assert/source_location.hpp>
#include <boost/cstdint.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <ostream>
#include <new>
#include <cstdio>
#include <system_error>

#if defined(BOOST_GCC) && BOOST_GCC >= 40600 && BOOST_GCC < 70000
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

namespace boost
{

namespace system
{

//  class error_code

//  We want error_code to be a value type that can be copied without slicing
//  and without requiring heap allocation, but we also want it to have
//  polymorphic behavior based on the error category. This is achieved by
//  abstract base class error_category supplying the polymorphic behavior,
//  and error_code containing a pointer to an object of a type derived
//  from error_category.

bool operator==( const error_code & code, const error_condition & condition ) noexcept;
std::size_t hash_value( error_code const & ec );

class error_code
{
private:

    friend bool operator==( const error_code & code, const error_condition & condition ) noexcept;
    friend std::size_t hash_value( error_code const & ec );

private:

    struct data
    {
        int val_;
        const error_category * cat_;
    };

    union
    {
        data d1_;
        unsigned char d2_[ sizeof(std::error_code) ];
    };

    // 0: default constructed, d1_ value initialized
    // 1: holds std::error_code in d2_
    // 2: holds error code in d1_, failed == false
    // 3: holds error code in d1_, failed == true
    // >3: pointer to source_location, failed_ in lsb
    boost::uintptr_t lc_flags_;

private:

    char const* category_name() const noexcept
    {
        // return category().name();

        if( lc_flags_ == 0 )
        {
            // must match detail::system_error_category::name()
            return "system";
        }
        else if( lc_flags_ == 1 )
        {
            // must match detail::interop_error_category::name()
            return "std:unknown";
        }
        else
        {
            return d1_.cat_->name();
        }
    }

public:

    // constructors:

    constexpr error_code() noexcept:
        d1_(), lc_flags_( 0 )
    {
    }

    BOOST_SYSTEM_CONSTEXPR error_code( int val, const error_category & cat ) noexcept:
        d1_(), lc_flags_( 2 + detail::failed_impl( val, cat ) )
    {
        d1_.val_ = val;
        d1_.cat_ = &cat;
    }

    error_code( int val, const error_category & cat, source_location const * loc ) noexcept:
        d1_(), lc_flags_( ( loc? reinterpret_cast<boost::uintptr_t>( loc ): 2 ) | +detail::failed_impl( val, cat ) )
    {
        d1_.val_ = val;
        d1_.cat_ = &cat;
    }

    template<class ErrorCodeEnum> BOOST_SYSTEM_CONSTEXPR error_code( ErrorCodeEnum e,
        typename detail::enable_if<
            is_error_code_enum<ErrorCodeEnum>::value
            || std::is_error_code_enum<ErrorCodeEnum>::value
        >::type* = 0 ) noexcept: d1_(), lc_flags_( 0 )
    {
        *this = make_error_code( e );
    }

    error_code( error_code const& ec, source_location const * loc ) noexcept:
        d1_(), lc_flags_( 0 )
    {
        *this = ec;

        if( ec.lc_flags_ != 0 && ec.lc_flags_ != 1 )
        {
            lc_flags_ = ( loc? reinterpret_cast<boost::uintptr_t>( loc ): 2 ) | ( ec.lc_flags_ & 1 );
        }
    }

    error_code( std::error_code const& ec ) noexcept:
        d1_(), lc_flags_( 0 )
    {
#ifndef BOOST_NO_RTTI

        if( detail::std_category const* pc2 = dynamic_cast< detail::std_category const* >( &ec.category() ) )
        {
            *this = boost::system::error_code( ec.value(), pc2->original_category() );
        }
        else

#endif
        {
            ::new( d2_ ) std::error_code( ec );
            lc_flags_ = 1;
        }
    }

    // modifiers:

    BOOST_SYSTEM_CONSTEXPR void assign( int val, const error_category & cat ) noexcept
    {
        *this = error_code( val, cat );
    }

    void assign( int val, const error_category & cat, source_location const * loc ) noexcept
    {
        *this = error_code( val, cat, loc );
    }

    void assign( error_code const& ec, source_location const * loc ) noexcept
    {
        *this = error_code( ec, loc );
    }

    template<typename ErrorCodeEnum>
        BOOST_SYSTEM_CONSTEXPR typename detail::enable_if<is_error_code_enum<ErrorCodeEnum>::value, error_code>::type &
        operator=( ErrorCodeEnum val ) noexcept
    {
        *this = make_error_code( val );
        return *this;
    }

    BOOST_SYSTEM_CONSTEXPR void clear() noexcept
    {
        *this = error_code();
    }

    // observers:

    BOOST_SYSTEM_CONSTEXPR int value() const noexcept
    {
        if( lc_flags_ != 1 )
        {
            return d1_.val_;
        }
        else
        {
            std::error_code const& ec = *reinterpret_cast<std::error_code const*>( d2_ );

            unsigned cv = static_cast<unsigned>( ec.value() );
            unsigned ch = static_cast<unsigned>( reinterpret_cast<boost::uintptr_t>( &ec.category() ) % 2097143 ); // 2^21-9, prime

            return static_cast<int>( cv + 1000 * ch );
        }
    }

    BOOST_SYSTEM_CONSTEXPR const error_category & category() const noexcept
    {
        if( lc_flags_ == 0 )
        {
            return system_category();
        }
        else if( lc_flags_ == 1 )
        {
            return detail::interop_category();
        }
        else
        {
            return *d1_.cat_;
        }
    }

    // deprecated?
    error_condition default_error_condition() const noexcept
    {
        return category().default_error_condition( value() );
    }

    std::string message() const
    {
        if( lc_flags_ == 1 )
        {
            std::error_code const& ec = *reinterpret_cast<std::error_code const*>( d2_ );
            return ec.message();
        }
        else if( lc_flags_ == 0 )
        {
            return detail::system_error_category_message( value() );
        }
        else
        {
            return category().message( value() );
        }
    }

    char const * message( char * buffer, std::size_t len ) const noexcept
    {
        if( lc_flags_ == 1 )
        {
            std::error_code const& ec = *reinterpret_cast<std::error_code const*>( d2_ );

#if !defined(BOOST_NO_EXCEPTIONS)
            try
#endif
            {
                detail::snprintf( buffer, len, "%s", ec.message().c_str() );
                return buffer;
            }
#if !defined(BOOST_NO_EXCEPTIONS)
            catch( ... )
            {
                detail::snprintf( buffer, len, "No message text available for error std:%s:%d", ec.category().name(), ec.value() );
                return buffer;
            }
#endif
        }
        else if( lc_flags_ == 0 )
        {
            return detail::system_error_category_message( value(), buffer, len );
        }
        else
        {
            return category().message( value(), buffer, len );
        }
    }

    BOOST_SYSTEM_CONSTEXPR bool failed() const noexcept
    {
        if( lc_flags_ & 1 )
        {
            if( lc_flags_ == 1 )
            {
                std::error_code const& ec = *reinterpret_cast<std::error_code const*>( d2_ );
                return ec.value() != 0;
            }

            return true;
        }
        else
        {
            return false;
        }
    }

    BOOST_SYSTEM_CONSTEXPR explicit operator bool() const noexcept  // true if error
    {
        return failed();
    }

    bool has_location() const noexcept
    {
        return lc_flags_ >= 4;
    }

    source_location const & location() const noexcept
    {
        BOOST_STATIC_CONSTEXPR source_location loc;
        return lc_flags_ >= 4? *reinterpret_cast<source_location const*>( lc_flags_ &~ static_cast<boost::uintptr_t>( 1 ) ): loc;
    }

    // relationals:

private:

    // private equality for use in error_category::equivalent

    friend class error_category;

    BOOST_SYSTEM_CONSTEXPR bool equals( int val, error_category const& cat ) const noexcept
    {
        if( lc_flags_ == 0 )
        {
            return val == 0 && cat.id_ == detail::system_category_id;
        }
        else if( lc_flags_ == 1 )
        {
            return cat.id_ == detail::interop_category_id && val == value();
        }
        else
        {
            return val == d1_.val_ && cat == *d1_.cat_;
        }
    }

public:

    //  the more symmetrical non-member syntax allows enum
    //  conversions work for both rhs and lhs.

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( const error_code & lhs, const error_code & rhs ) noexcept
    {
        bool s1 = lhs.lc_flags_ == 1;
        bool s2 = rhs.lc_flags_ == 1;

        if( s1 != s2 ) return false;

        if( s1 && s2 )
        {
            std::error_code const& e1 = *reinterpret_cast<std::error_code const*>( lhs.d2_ );
            std::error_code const& e2 = *reinterpret_cast<std::error_code const*>( rhs.d2_ );

            return e1 == e2;
        }
        else
        {
            return lhs.value() == rhs.value() && lhs.category() == rhs.category();
        }
    }

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator<( const error_code & lhs, const error_code & rhs ) noexcept
    {
        bool s1 = lhs.lc_flags_ == 1;
        bool s2 = rhs.lc_flags_ == 1;

        if( s1 < s2 ) return true;
        if( s2 < s1 ) return false;

        if( s1 && s2 )
        {
            std::error_code const& e1 = *reinterpret_cast<std::error_code const*>( lhs.d2_ );
            std::error_code const& e2 = *reinterpret_cast<std::error_code const*>( rhs.d2_ );

            return e1 < e2;
        }
        else
        {
            return lhs.category() < rhs.category() || (lhs.category() == rhs.category() && lhs.value() < rhs.value());
        }
    }

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator!=( const error_code & lhs, const error_code & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    inline friend bool operator==( std::error_code const & lhs, error_code const & rhs ) noexcept
    {
        return lhs == static_cast< std::error_code >( rhs );
    }

    inline friend bool operator==( error_code const & lhs, std::error_code const & rhs ) noexcept
    {
        return static_cast< std::error_code >( lhs ) == rhs;
    }

    inline friend bool operator!=( std::error_code const & lhs, error_code const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    inline friend bool operator!=( error_code const & lhs, std::error_code const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    //

    template<class E, class N = typename detail::enable_if<std::is_error_condition_enum<E>::value>::type>
    inline friend bool operator==( error_code const & lhs, E rhs ) noexcept
    {
        return lhs == make_error_condition( rhs );
    }

    template<class E, class N = typename detail::enable_if<std::is_error_condition_enum<E>::value>::type>
    inline friend bool operator==( E lhs, error_code const & rhs ) noexcept
    {
        return make_error_condition( lhs ) == rhs;
    }

    template<class E, class N = typename detail::enable_if<std::is_error_condition_enum<E>::value>::type>
    inline friend bool operator!=( error_code const & lhs, E rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    template<class E, class N = typename detail::enable_if<std::is_error_condition_enum<E>::value>::type>
    inline friend bool operator!=( E lhs, error_code const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    //

    template<class E, class N1 = void, class N2 = typename detail::enable_if<std::is_error_code_enum<E>::value>::type>
    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( error_code const & lhs, E rhs ) noexcept
    {
        return lhs == make_error_code( rhs );
    }

    template<class E, class N1 = void, class N2 = typename detail::enable_if<std::is_error_code_enum<E>::value>::type>
    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( E lhs, error_code const & rhs ) noexcept
    {
        return make_error_code( lhs ) == rhs;
    }

    template<class E, class N1 = void, class N2 = typename detail::enable_if<std::is_error_code_enum<E>::value>::type>
    BOOST_SYSTEM_CONSTEXPR inline friend bool operator!=( error_code const & lhs, E rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    template<class E, class N1 = void, class N2 = typename detail::enable_if<std::is_error_code_enum<E>::value>::type>
    BOOST_SYSTEM_CONSTEXPR inline friend bool operator!=( E lhs, error_code const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

#if defined(BOOST_SYSTEM_CLANG_6)

    inline friend bool operator==( error_code const & lhs, std::error_condition const & rhs ) noexcept
    {
        return static_cast< std::error_code >( lhs ) == rhs;
    }

    inline friend bool operator==( std::error_condition const & lhs, error_code const & rhs ) noexcept
    {
        return lhs == static_cast< std::error_code >( rhs );
    }

    inline friend bool operator!=( error_code const & lhs, std::error_condition const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    inline friend bool operator!=( std::error_condition const & lhs, error_code const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

#endif

    // conversions

    operator std::error_code () const
    {
        if( lc_flags_ == 1 )
        {
            return *reinterpret_cast<std::error_code const*>( d2_ );
        }
        else if( lc_flags_ == 0 )
        {
// This condition must be the same as the one in error_category_impl.hpp
#if defined(BOOST_SYSTEM_AVOID_STD_SYSTEM_CATEGORY)

            return std::error_code( 0, boost::system::system_category() );

#else

            return std::error_code();

#endif
        }
        else
        {
            return std::error_code( d1_.val_, *d1_.cat_ );
        }
    }

    operator std::error_code ()
    {
        return const_cast<error_code const&>( *this );
    }

    template<class T,
      class E = typename detail::enable_if<detail::is_same<T, std::error_code>::value>::type>
      operator T& ()
    {
        if( lc_flags_ != 1 )
        {
            std::error_code e2( *this );
            ::new( d2_ ) std::error_code( e2 );
            lc_flags_ = 1;
        }

        return *reinterpret_cast<std::error_code*>( d2_ );
    }

#if defined(BOOST_SYSTEM_CLANG_6)

    template<class T,
      class E = typename std::enable_if<std::is_same<T, std::error_code>::value>::type>
      operator T const& () = delete;

#endif

    std::string to_string() const
    {
        if( lc_flags_ == 1 )
        {
            std::error_code const& e2 = *reinterpret_cast<std::error_code const*>( d2_ );

            std::string r( "std:" );
            r += e2.category().name();
            detail::append_int( r, e2.value() );

            return r;
        }
        else
        {
            std::string r = category_name();
            detail::append_int( r, value() );
            return r;
        }
    }

    template<class Ch, class Tr>
        inline friend std::basic_ostream<Ch, Tr>&
        operator<< (std::basic_ostream<Ch, Tr>& os, error_code const & ec)
    {
        return os << ec.to_string().c_str();
    }

    std::string what() const
    {
        std::string r = message();

        r += " [";
        r += to_string();

        if( has_location() )
        {
            r += " at ";
            r += location().to_string();
        }

        r += "]";
        return r;
    }
};

inline bool operator==( const error_code & code, const error_condition & condition ) noexcept
{
    if( code.lc_flags_ == 1 )
    {
        return static_cast<std::error_code>( code ) == static_cast<std::error_condition>( condition );
    }
    else
    {
        return code.category().equivalent( code.value(), condition ) || condition.category().equivalent( code, condition.value() );
    }
}

inline bool operator==( const error_condition & condition, const error_code & code ) noexcept
{
    return code == condition;
}

inline bool operator!=( const error_code & lhs, const error_condition & rhs ) noexcept
{
    return !( lhs == rhs );
}

inline bool operator!=( const error_condition & lhs, const error_code & rhs ) noexcept
{
    return !( lhs == rhs );
}

inline std::size_t hash_value( error_code const & ec )
{
    if( ec.lc_flags_ == 1 )
    {
        std::error_code const& e2 = *reinterpret_cast<std::error_code const*>( ec.d2_ );
        return std::hash<std::error_code>()( e2 );
    }

    error_category const & cat = ec.category();

    boost::ulong_long_type id_ = cat.id_;

    if( id_ == 0 )
    {
        id_ = reinterpret_cast<boost::uintptr_t>( &cat );
    }

    boost::ulong_long_type hv = ( boost::ulong_long_type( 0xCBF29CE4 ) << 32 ) + 0x84222325;
    boost::ulong_long_type const prime = ( boost::ulong_long_type( 0x00000100 ) << 32 ) + 0x000001B3;

    // id

    hv ^= id_;
    hv *= prime;

    // value

    hv ^= static_cast<unsigned>( ec.value() );
    hv *= prime;

    return static_cast<std::size_t>( hv );
}

} // namespace system

} // namespace boost

#if defined(BOOST_GCC) && BOOST_GCC >= 40600 && BOOST_GCC < 70000
# pragma GCC diagnostic pop
#endif

#endif // #ifndef BOOST_SYSTEM_DETAIL_ERROR_CODE_HPP_INCLUDED
