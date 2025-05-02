#ifndef BOOST_CORE_VERBOSE_TERMINATE_HANDLER_HPP_INCLUDED
#define BOOST_CORE_VERBOSE_TERMINATE_HANDLER_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//  Copyright 2022 Peter Dimov
//  Distributed under the Boost Software License, Version 1.0.
//  https://www.boost.org/LICENSE_1_0.txt

#include <boost/core/demangle.hpp>
#include <boost/throw_exception.hpp>
#include <boost/config.hpp>
#include <exception>
#include <typeinfo>
#include <cstdlib>
#include <cstdio>

namespace boost
{
namespace core
{

BOOST_NORETURN inline void verbose_terminate_handler()
{
    std::set_terminate( 0 );

#if defined(BOOST_NO_EXCEPTIONS)

    std::fputs( "std::terminate called with exceptions disabled\n", stderr );

#else

    try
    {
        throw;
    }
    catch( std::exception const& x )
    {
#if defined(BOOST_NO_RTTI)

        char const * typeid_name = "unknown (RTTI is disabled)";

#else

        char const * typeid_name = typeid( x ).name();

        boost::core::scoped_demangled_name typeid_demangled_name( typeid_name );

        if( typeid_demangled_name.get() != 0 )
        {
            typeid_name = typeid_demangled_name.get();
        }

#endif

        boost::source_location loc = boost::get_throw_location( x );

        std::fprintf( stderr,
            "std::terminate called after throwing an exception:\n\n"
            "      type: %s\n"
            "    what(): %s\n"
            "  location: %s:%lu:%lu in function '%s'\n",

            typeid_name,
            x.what(),
            loc.file_name(), static_cast<unsigned long>( loc.line() ),
            static_cast<unsigned long>( loc.column() ), loc.function_name()
        );
    }
    catch( ... )
    {
        std::fputs( "std::terminate called after throwing an unknown exception\n", stderr );
    }

#endif

    std::fflush( stdout );
    std::abort();
}

} // namespace core
} // namespace boost

#endif  // #ifndef BOOST_CORE_VERBOSE_TERMINATE_HANDLER_HPP_INCLUDED
