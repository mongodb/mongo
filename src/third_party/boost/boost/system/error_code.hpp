#ifndef BOOST_SYSTEM_ERROR_CODE_HPP_INCLUDED
#define BOOST_SYSTEM_ERROR_CODE_HPP_INCLUDED

//  Copyright Beman Dawes 2006, 2007
//  Copyright Christoper Kohlhoff 2007
//  Copyright Peter Dimov 2017, 2018
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/error_code.hpp>
#include <boost/system/error_category.hpp>
#include <boost/system/error_condition.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/generic_category.hpp>
#include <boost/system/system_category.hpp>
#include <boost/system/detail/throws.hpp>
#include <boost/config.hpp>

namespace boost
{

namespace system
{

// non-member functions of error_code and error_condition

inline bool operator==( const error_code & code, const error_condition & condition ) BOOST_NOEXCEPT
{
    return code.category().equivalent( code.value(), condition ) || condition.category().equivalent( code, condition.value() );
}

inline bool operator!=( const error_code & lhs, const error_condition & rhs ) BOOST_NOEXCEPT
{
    return !( lhs == rhs );
}

inline bool operator==( const error_condition & condition, const error_code & code ) BOOST_NOEXCEPT
{
    return code.category().equivalent( code.value(), condition ) || condition.category().equivalent( code, condition.value() );
}

inline bool operator!=( const error_condition & lhs, const error_code & rhs ) BOOST_NOEXCEPT
{
    return !( lhs == rhs );
}

} // namespace system

} // namespace boost

#endif // BOOST_SYSTEM_ERROR_CODE_HPP_INCLUDED
