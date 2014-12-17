//  scoped_enum_emulation.hpp  ---------------------------------------------------------//

//  Copyright Beman Dawes, 2009

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Generates C++0x scoped enums if the feature is present, otherwise emulates C++0x
//  scoped enums with C++03 namespaces and enums. The Boost.Config BOOST_NO_SCOPED_ENUMS
//  macro is used to detect feature support.
//
//  See http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2347.pdf for a
//  description of the scoped enum feature. Note that the committee changed the name
//  from strongly typed enum to scoped enum.  
//
//  Caution: only the syntax is emulated; the semantics are not emulated and
//  the syntax emulation doesn't include being able to specify the underlying
//  representation type.
//
//  The emulation is via struct rather than namespace to allow use within classes.
//  Thanks to Andrey Semashev for pointing that out.
//
//  Helpful comments and suggestions were also made by Kjell Elster, Phil Endecott,
//  Joel Falcou, Mathias Gaunard, Felipe Magno de Almeida, Matt Calabrese, Vincente
//  Botet, and Daniel James. 
//
//  Sample usage:
//
//     BOOST_SCOPED_ENUM_START(algae) { green, red, cyan }; BOOST_SCOPED_ENUM_END
//     ...
//     BOOST_SCOPED_ENUM(algae) sample( algae::red );
//     void foo( BOOST_SCOPED_ENUM(algae) color );
//     ...
//     sample = algae::green;
//     foo( algae::cyan );

#ifndef BOOST_SCOPED_ENUM_EMULATION_HPP
#define BOOST_SCOPED_ENUM_EMULATION_HPP

#include <boost/config.hpp>

#ifdef BOOST_NO_SCOPED_ENUMS

# define BOOST_SCOPED_ENUM_START(name) struct name { enum enum_type
# define BOOST_SCOPED_ENUM_END };
# define BOOST_SCOPED_ENUM(name) name::enum_type

#else

# define BOOST_SCOPED_ENUM_START(name) enum class name
# define BOOST_SCOPED_ENUM_END
# define BOOST_SCOPED_ENUM(name) name

#endif

#endif  // BOOST_SCOPED_ENUM_EMULATION_HPP
