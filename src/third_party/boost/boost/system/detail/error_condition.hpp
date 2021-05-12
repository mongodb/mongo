#ifndef BOOST_SYSTEM_DETAIL_ERROR_CONDITION_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_ERROR_CONDITION_HPP_INCLUDED

//  Copyright Beman Dawes 2006, 2007
//  Copyright Christoper Kohlhoff 2007
//  Copyright Peter Dimov 2017, 2018
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/error_category.hpp>
#include <boost/system/detail/generic_category.hpp>
#include <boost/system/detail/enable_if.hpp>
#include <boost/system/is_error_condition_enum.hpp>
#include <boost/system/detail/config.hpp>
#include <boost/config.hpp>

namespace boost
{

namespace system
{

// class error_condition

// error_conditions are portable, error_codes are system or library specific

class error_condition
{
private:

    int val_;
    error_category const * cat_;

public:

    // constructors:

    BOOST_SYSTEM_CONSTEXPR error_condition() BOOST_NOEXCEPT:
        val_( 0 ), cat_( &generic_category() )
    {
    }

    BOOST_SYSTEM_CONSTEXPR error_condition( int val, const error_category & cat ) BOOST_NOEXCEPT:
        val_( val ), cat_( &cat )
    {
    }

    template<class ErrorConditionEnum> BOOST_SYSTEM_CONSTEXPR error_condition( ErrorConditionEnum e,
        typename detail::enable_if<is_error_condition_enum<ErrorConditionEnum>::value>::type* = 0) BOOST_NOEXCEPT
    {
        *this = make_error_condition( e );
    }

    // modifiers:

    BOOST_SYSTEM_CONSTEXPR void assign( int val, const error_category & cat ) BOOST_NOEXCEPT
    {
        val_ = val;
        cat_ = &cat;
    }

    template<typename ErrorConditionEnum>
        BOOST_SYSTEM_CONSTEXPR typename detail::enable_if<is_error_condition_enum<ErrorConditionEnum>::value, error_condition>::type &
        operator=( ErrorConditionEnum val ) BOOST_NOEXCEPT
    {
        *this = make_error_condition( val );
        return *this;
    }

    BOOST_SYSTEM_CONSTEXPR void clear() BOOST_NOEXCEPT
    {
        val_ = 0;
        cat_ = &generic_category();
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

    std::string message() const
    {
        return cat_->message( value() );
    }

    BOOST_SYSTEM_DEPRECATED("this function is slated for removal") char const * message( char * buffer, std::size_t len ) const BOOST_NOEXCEPT
    {
        return cat_->message( value(), buffer, len );
    }

    BOOST_SYSTEM_DEPRECATED("this function is slated for removal") BOOST_SYSTEM_CONSTEXPR bool failed() const BOOST_NOEXCEPT
    {
        return detail::failed_impl( val_, *cat_ );
    }

#if !defined(BOOST_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS)

    BOOST_SYSTEM_CONSTEXPR explicit operator bool() const BOOST_NOEXCEPT  // true if error
    {
        return val_ != 0;
    }

#else

    typedef void (*unspecified_bool_type)();
    static void unspecified_bool_true() {}

    BOOST_SYSTEM_CONSTEXPR operator unspecified_bool_type() const BOOST_NOEXCEPT  // true if error
    {
        return val_ != 0? unspecified_bool_true: 0;
    }

    BOOST_SYSTEM_CONSTEXPR bool operator!() const BOOST_NOEXCEPT  // true if no error
    {
        return val_ == 0;
    }

#endif

    // relationals:
    //  the more symmetrical non-member syntax allows enum
    //  conversions work for both rhs and lhs.

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( const error_condition & lhs, const error_condition & rhs ) BOOST_NOEXCEPT
    {
        return lhs.val_ == rhs.val_ && *lhs.cat_ == *rhs.cat_;
    }

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator<( const error_condition & lhs, const error_condition & rhs ) BOOST_NOEXCEPT
    {
        return *lhs.cat_ < *rhs.cat_ || ( *lhs.cat_ == *rhs.cat_ && lhs.val_ < rhs.val_ );
    }

#if defined(BOOST_SYSTEM_HAS_SYSTEM_ERROR)

    operator std::error_condition () const
    {
        return std::error_condition( value(), category() );
    }

#endif
};

BOOST_SYSTEM_CONSTEXPR inline bool operator!=( const error_condition & lhs, const error_condition & rhs ) BOOST_NOEXCEPT
{
    return !( lhs == rhs );
}

} // namespace system

} // namespace boost

#endif // #ifndef BOOST_SYSTEM_DETAIL_ERROR_CONDITION_HPP_INCLUDED
