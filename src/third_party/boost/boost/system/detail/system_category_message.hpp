#ifndef BOOST_SYSTEM_DETAIL_SYSTEM_CATEGORY_MESSAGE_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_SYSTEM_CATEGORY_MESSAGE_HPP_INCLUDED

// Implementation of system_error_category_message
//
// Copyright 2018, 2022 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See library home page at http://www.boost.org/libs/system

#include <boost/system/api_config.hpp>

#if !defined(BOOST_POSIX_API) && !defined(BOOST_WINDOWS_API)
#  error BOOST_POSIX_API or BOOST_WINDOWS_API must be defined
#endif

#if defined(BOOST_WINDOWS_API)

#include <boost/system/detail/system_category_message_win32.hpp>

namespace boost
{
namespace system
{
namespace detail
{

inline std::string system_error_category_message( int ev )
{
    return system_category_message_win32( ev );
}

inline char const * system_error_category_message( int ev, char * buffer, std::size_t len ) BOOST_NOEXCEPT
{
    return system_category_message_win32( ev, buffer, len );
}

} // namespace detail
} // namespace system
} // namespace boost

#else // #if defined(BOOST_WINDOWS_API)

#include <boost/system/detail/generic_category_message.hpp>

namespace boost
{
namespace system
{
namespace detail
{

inline std::string system_error_category_message( int ev )
{
    return generic_error_category_message( ev );
}

inline char const * system_error_category_message( int ev, char * buffer, std::size_t len ) BOOST_NOEXCEPT
{
    return generic_error_category_message( ev, buffer, len );
}

} // namespace detail
} // namespace system
} // namespace boost

#endif // #if defined(BOOST_WINDOWS_API)

#endif // #ifndef BOOST_SYSTEM_DETAIL_SYSTEM_CATEGORY_MESSAGE_HPP_INCLUDED
