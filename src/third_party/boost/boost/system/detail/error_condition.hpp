#ifndef BOOST_SYSTEM_DETAIL_ERROR_CONDITION_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_ERROR_CONDITION_HPP_INCLUDED

//  Copyright Beman Dawes 2006, 2007
//  Copyright Christoper Kohlhoff 2007
//  Copyright Peter Dimov 2017-2021
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/error_category.hpp>
#include <boost/system/detail/generic_category.hpp>
#include <boost/system/detail/enable_if.hpp>
#include <boost/system/detail/is_same.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/append_int.hpp>
#include <boost/system/is_error_condition_enum.hpp>
#include <boost/system/detail/config.hpp>
#include <boost/config.hpp>

namespace boost
{

namespace system
{

// class error_condition

// error_conditions are portable, error_codes are system or library specific

namespace detail
{

struct generic_value_tag
{
    int value;
    BOOST_SYSTEM_CONSTEXPR explicit generic_value_tag( int v ): value( v ) {}
};

} // namespace detail

class error_condition
{
private:

    int val_;
    error_category const * cat_;

private:

    boost::ulong_long_type cat_id() const noexcept
    {
        return cat_? cat_->id_: detail::generic_category_id;
    }

public:

    // constructors:

    BOOST_SYSTEM_CONSTEXPR error_condition() noexcept:
        val_( 0 ), cat_( 0 )
    {
    }

    BOOST_SYSTEM_CONSTEXPR error_condition( int val, const error_category & cat ) noexcept:
        val_( val ), cat_( &cat )
    {
    }

    BOOST_SYSTEM_CONSTEXPR explicit error_condition( boost::system::detail::generic_value_tag vt ) noexcept:
        val_( vt.value ), cat_( 0 )
    {
    }

    template<class ErrorConditionEnum> BOOST_SYSTEM_CONSTEXPR error_condition( ErrorConditionEnum e,
      typename detail::enable_if<
        is_error_condition_enum<ErrorConditionEnum>::value && !boost::system::detail::is_same<ErrorConditionEnum, errc::errc_t>::value
      >::type* = 0) noexcept
    {
        *this = make_error_condition( e );
    }

    template<class ErrorConditionEnum> BOOST_SYSTEM_CONSTEXPR error_condition( ErrorConditionEnum e,
      typename detail::enable_if<boost::system::detail::is_same<ErrorConditionEnum, errc::errc_t>::value>::type* = 0) noexcept:
        val_( e ), cat_( 0 )
    {
    }

    // modifiers:

    BOOST_SYSTEM_CONSTEXPR void assign( int val, const error_category & cat ) noexcept
    {
        val_ = val;
        cat_ = &cat;
    }

    template<typename ErrorConditionEnum>
        BOOST_SYSTEM_CONSTEXPR typename detail::enable_if<is_error_condition_enum<ErrorConditionEnum>::value, error_condition>::type &
        operator=( ErrorConditionEnum val ) noexcept
    {
        *this = error_condition( val );
        return *this;
    }

    BOOST_SYSTEM_CONSTEXPR void clear() noexcept
    {
        val_ = 0;
        cat_ = 0;
    }

    // observers:

    BOOST_SYSTEM_CONSTEXPR int value() const noexcept
    {
        return val_;
    }

    BOOST_SYSTEM_CONSTEXPR const error_category & category() const noexcept
    {
        return cat_? *cat_: generic_category();
    }

    std::string message() const
    {
        if( cat_ )
        {
            return cat_->message( value() );
        }
        else
        {
            return detail::generic_error_category_message( value() );
        }
    }

    char const * message( char * buffer, std::size_t len ) const noexcept
    {
        if( cat_ )
        {
            return cat_->message( value(), buffer, len );
        }
        else
        {
            return detail::generic_error_category_message( value(), buffer, len );
        }
    }

    BOOST_SYSTEM_CONSTEXPR bool failed() const noexcept
    {
        if( cat_ )
        {
            return detail::failed_impl( val_, *cat_ );
        }
        else
        {
            return val_ != 0;
        }
    }

    BOOST_SYSTEM_CONSTEXPR explicit operator bool() const noexcept  // true if error
    {
        return failed();
    }

    // relationals:
    //  the more symmetrical non-member syntax allows enum
    //  conversions work for both rhs and lhs.

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( const error_condition & lhs, const error_condition & rhs ) noexcept
    {
        if( lhs.val_ != rhs.val_ )
        {
            return false;
        }
        else if( lhs.cat_ == 0 )
        {
            return rhs.cat_id() == detail::generic_category_id;
        }
        else if( rhs.cat_ == 0 )
        {
            return lhs.cat_id() == detail::generic_category_id;
        }
        else
        {
            return *lhs.cat_ == *rhs.cat_;
        }
    }

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator<( const error_condition & lhs, const error_condition & rhs ) noexcept
    {
        error_category const& lcat = lhs.category();
        error_category const& rcat = rhs.category();
        return lcat < rcat || ( lcat == rcat && lhs.val_ < rhs.val_ );
    }

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator!=( const error_condition & lhs, const error_condition & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    operator std::error_condition () const
    {
// This condition must be the same as the one in error_category_impl.hpp
#if defined(BOOST_SYSTEM_AVOID_STD_GENERIC_CATEGORY)

        return std::error_condition( value(), category() );

#else

        if( cat_ )
        {
            return std::error_condition( val_, *cat_ );
        }
        else
        {
            return std::error_condition( val_, std::generic_category() );
        }

#endif
    }

    inline friend bool operator==( std::error_code const & lhs, error_condition const & rhs ) noexcept
    {
        return lhs == static_cast< std::error_condition >( rhs );
    }

    inline friend bool operator==( error_condition const & lhs, std::error_code const & rhs ) noexcept
    {
        return static_cast< std::error_condition >( lhs ) == rhs;
    }

    inline friend bool operator!=( std::error_code const & lhs, error_condition const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    inline friend bool operator!=( error_condition const & lhs, std::error_code const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    //

    template<class E, class N = typename detail::enable_if<std::is_error_condition_enum<E>::value>::type>
    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( error_condition const & lhs, E rhs ) noexcept
    {
        return lhs == make_error_condition( rhs );
    }

    template<class E, class N = typename detail::enable_if<std::is_error_condition_enum<E>::value>::type>
    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( E lhs, error_condition const & rhs ) noexcept
    {
        return make_error_condition( lhs ) == rhs;
    }

    template<class E, class N = typename detail::enable_if<std::is_error_condition_enum<E>::value>::type>
    BOOST_SYSTEM_CONSTEXPR inline friend bool operator!=( error_condition const & lhs, E rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    template<class E, class N = typename detail::enable_if<std::is_error_condition_enum<E>::value>::type>
    BOOST_SYSTEM_CONSTEXPR inline friend bool operator!=( E lhs, error_condition const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    //

    template<class E, class N1 = void, class N2 = typename detail::enable_if<std::is_error_code_enum<E>::value>::type>
    inline friend bool operator==( error_condition const & lhs, E rhs ) noexcept
    {
        return lhs == make_error_code( rhs );
    }

    template<class E, class N1 = void, class N2 = typename detail::enable_if<std::is_error_code_enum<E>::value>::type>
    inline friend bool operator==( E lhs, error_condition const & rhs ) noexcept
    {
        return make_error_code( lhs ) == rhs;
    }

    template<class E, class N1 = void, class N2 = typename detail::enable_if<std::is_error_code_enum<E>::value>::type>
    inline friend bool operator!=( error_condition const & lhs, E rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    template<class E, class N1 = void, class N2 = typename detail::enable_if<std::is_error_code_enum<E>::value>::type>
    inline friend bool operator!=( E lhs, error_condition const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    std::string to_string() const
    {
        std::string r( "cond:" );

        if( cat_ )
        {
            r += cat_->name();
        }
        else
        {
            r += "generic";
        }

        detail::append_int( r, value() );

        return r;
    }

    template<class Ch, class Tr>
        inline friend std::basic_ostream<Ch, Tr>&
        operator<< (std::basic_ostream<Ch, Tr>& os, error_condition const & en)
    {
        os << en.to_string();
        return os;
    }
};

} // namespace system

} // namespace boost

#endif // #ifndef BOOST_SYSTEM_DETAIL_ERROR_CONDITION_HPP_INCLUDED
