#ifndef BOOST_SYSTEM_SYSTEM_ERROR_HPP
#define BOOST_SYSTEM_SYSTEM_ERROR_HPP

// Copyright Beman Dawes 2006
// Copyright Peter Dimov 2021
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/system/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <string>
#include <stdexcept>
#include <cassert>

namespace boost
{
namespace system
{

class BOOST_SYMBOL_VISIBLE system_error: public std::runtime_error
{
private:

    error_code code_;

public:

    explicit system_error( error_code const & ec ):
        std::runtime_error( ec.what() ), code_( ec ) {}

    system_error( error_code const & ec, std::string const & prefix ):
        std::runtime_error( prefix + ": " + ec.what() ), code_( ec ) {}

    system_error( error_code const & ec, char const * prefix ):
        std::runtime_error( std::string( prefix ) + ": " + ec.what() ), code_( ec ) {}

    system_error( int ev, error_category const & ecat ):
        std::runtime_error( error_code( ev, ecat ).what() ), code_( ev, ecat ) {}

    system_error( int ev, error_category const & ecat, std::string const & prefix ):
        std::runtime_error( prefix + ": " + error_code( ev, ecat ).what() ), code_( ev, ecat ) {}

    system_error( int ev, error_category const & ecat, char const * prefix ):
        std::runtime_error( std::string( prefix ) + ": " + error_code( ev, ecat ).what() ), code_( ev, ecat ) {}

    error_code code() const noexcept
    {
        return code_;
    }
};

} // namespace system
} // namespace boost

#endif // BOOST_SYSTEM_SYSTEM_ERROR_HPP
