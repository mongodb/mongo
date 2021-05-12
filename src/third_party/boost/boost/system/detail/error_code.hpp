#ifndef BOOST_SYSTEM_DETAIL_ERROR_CODE_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_ERROR_CODE_HPP_INCLUDED

//  Copyright Beman Dawes 2006, 2007
//  Copyright Christoper Kohlhoff 2007
//  Copyright Peter Dimov 2017, 2018
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/error_category.hpp>
#include <boost/system/detail/error_condition.hpp>
#include <boost/system/detail/system_category.hpp>
#include <boost/system/detail/enable_if.hpp>
#include <boost/system/is_error_code_enum.hpp>
#include <boost/system/detail/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/config.hpp>
#include <ostream>

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

class error_code
{
private:

    int val_;
    bool failed_;
    const error_category * cat_;

public:

    // constructors:

    BOOST_SYSTEM_CONSTEXPR error_code() BOOST_NOEXCEPT:
        val_( 0 ), failed_( false ), cat_( &system_category() )
    {
    }

    BOOST_SYSTEM_CONSTEXPR error_code( int val, const error_category & cat ) BOOST_NOEXCEPT:
        val_( val ), failed_( detail::failed_impl( val, cat ) ), cat_( &cat )
    {
    }

    template<class ErrorCodeEnum> BOOST_SYSTEM_CONSTEXPR error_code( ErrorCodeEnum e,
        typename detail::enable_if<is_error_code_enum<ErrorCodeEnum>::value>::type* = 0 ) BOOST_NOEXCEPT
    {
        *this = make_error_code( e );
    }

    // modifiers:

    BOOST_SYSTEM_CONSTEXPR void assign( int val, const error_category & cat ) BOOST_NOEXCEPT
    {
        val_ = val;
        failed_ = detail::failed_impl( val, cat );
        cat_ = &cat;
    }

    template<typename ErrorCodeEnum>
        BOOST_SYSTEM_CONSTEXPR typename detail::enable_if<is_error_code_enum<ErrorCodeEnum>::value, error_code>::type &
        operator=( ErrorCodeEnum val ) BOOST_NOEXCEPT
    {
        *this = make_error_code( val );
        return *this;
    }

    BOOST_SYSTEM_CONSTEXPR void clear() BOOST_NOEXCEPT
    {
        val_ = 0;
        failed_ = false;
        cat_ = &system_category();
    }

    // observers:

    BOOST_SYSTEM_CONSTEXPR int value() const BOOST_NOEXCEPT
    {
        return val_;
    }

    BOOST_SYSTEM_CONSTEXPR const error_category & category() const BOOST_NOEXCEPT
    {
        return *cat_;
    }

    error_condition default_error_condition() const BOOST_NOEXCEPT
    {
        return cat_->default_error_condition( value() );
    }

    std::string message() const
    {
        return cat_->message( value() );
    }

    char const * message( char * buffer, std::size_t len ) const BOOST_NOEXCEPT
    {
        return cat_->message( value(), buffer, len );
    }

    BOOST_SYSTEM_CONSTEXPR bool failed() const BOOST_NOEXCEPT
    {
        return failed_;
    }

#if !defined(BOOST_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS)

    BOOST_SYSTEM_CONSTEXPR explicit operator bool() const BOOST_NOEXCEPT  // true if error
    {
        return failed_;
    }

#else

    typedef void (*unspecified_bool_type)();
    static void unspecified_bool_true() {}

    BOOST_SYSTEM_CONSTEXPR operator unspecified_bool_type() const  BOOST_NOEXCEPT // true if error
    {
        return failed_? unspecified_bool_true: 0;
    }

    BOOST_SYSTEM_CONSTEXPR bool operator!() const BOOST_NOEXCEPT // true if no error
    {
        return !failed_;
    }

#endif

    // relationals:

    //  the more symmetrical non-member syntax allows enum
    //  conversions work for both rhs and lhs.

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( const error_code & lhs, const error_code & rhs ) BOOST_NOEXCEPT
    {
        return lhs.val_ == rhs.val_ && *lhs.cat_ == *rhs.cat_;
    }

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator<( const error_code & lhs, const error_code & rhs ) BOOST_NOEXCEPT
    {
        return *lhs.cat_ < *rhs.cat_ || ( *lhs.cat_ == *rhs.cat_ && lhs.val_ < rhs.val_ );
    }

#if defined(BOOST_SYSTEM_HAS_SYSTEM_ERROR)

    operator std::error_code () const
    {
        return std::error_code( value(), category() );
    }

#endif
};

BOOST_SYSTEM_CONSTEXPR inline bool operator!=( const error_code & lhs, const error_code & rhs ) BOOST_NOEXCEPT
{
    return !( lhs == rhs );
}

template<class Ch, class Tr>
    inline std::basic_ostream<Ch, Tr>&
    operator<< (std::basic_ostream<Ch, Tr>& os, error_code const & ec)
{
    os << ec.category().name() << ':' << ec.value();
    return os;
}

inline std::size_t hash_value( error_code const & ec )
{
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

#endif // #ifndef BOOST_SYSTEM_DETAIL_ERROR_CODE_HPP_INCLUDED
