#ifndef BOOST_COMPAT_DETAIL_THROW_SYSTEM_ERROR_HPP_INCLUDED
#define BOOST_COMPAT_DETAIL_THROW_SYSTEM_ERROR_HPP_INCLUDED

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/throw_exception.hpp>
#include <boost/config.hpp>
#include <system_error>

namespace boost {
namespace compat {
namespace detail {

BOOST_NORETURN BOOST_NOINLINE inline void throw_system_error( std::errc e, boost::source_location const& loc = BOOST_CURRENT_LOCATION )
{
    boost::throw_exception( std::system_error( std::make_error_code( e ) ), loc );
}

} // namespace detail
} // namespace compat
} // namespace boost

#endif // BOOST_COMPAT_DETAIL_THROW_SYSTEM_ERROR_HPP_INCLUDED
