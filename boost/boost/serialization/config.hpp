#ifndef BOOST_SERIALIZATION_CONFIG_HPP
#define BOOST_SERIALIZATION_CONFIG_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//  config.hpp  ---------------------------------------------//

//  © Copyright Robert Ramey 2004
//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/serialization

//----------------------------------------------------------------------------// 

// This header implements separate compilation features as described in
// http://www.boost.org/more/separate_compilation.html

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/preprocessor/facilities/empty.hpp>

// note: this version incorporates the related code into the the 
// the same library as BOOST_ARCHIVE.  This could change some day in the
// future

#ifdef BOOST_HAS_DECLSPEC // defined in config system
// we need to import/export our code only if the user has specifically
// asked for it by defining either BOOST_ALL_DYN_LINK if they want all boost
// libraries to be dynamically linked, or BOOST_SERIALIZATION_DYN_LINK
// if they want just this one to be dynamically liked:
#if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_SERIALIZATION_DYN_LINK)
#define BOOST_DYN_LINK
// export if this is our own source, otherwise import:
#if defined(BOOST_SERIALIZATION_SOURCE)
    #if defined(BOOST_MSVC) || defined(BOOST_INTEL_WIN) || defined(__MWERKS__)
    #define BOOST_SERIALIZATION_DECL(T) __declspec(dllexport) T
    #else
    #define BOOST_SERIALIZATION_DECL(T) T __declspec(dllexport)
    #endif
    #pragma message( "BOOST_SERIALIZATION_DECL __declspec(dllexport)" )
#endif // defined(BOOST_SERIALIZATION_SOURCE)
#endif // defined(BOOST_ALL_DYN_LINK) || defined(BOOST_SERIALIZATION_DYN_LINK)
#endif // BOOST_HAS_DECLSPEC

// if BOOST_SERIALIZATION_DECL isn't defined yet define it now:
#ifndef BOOST_SERIALIZATION_DECL
    #define BOOST_SERIALIZATION_DECL(T) T
#endif

//  enable automatic library variant selection  ------------------------------// 

#if !defined(BOOST_ALL_NO_LIB) && !defined(BOOST_SERIALIZATION_NO_LIB) \
&& !defined(BOOST_SERIALIZATION_SOURCE) \
&& !defined(BOOST_ARCHIVE_SOURCE) && !defined(BOOST_WARCHIVE_SOURCE)
//
// Set the name of our library, this will get undef'ed by auto_link.hpp
// once it's done with it:
//
#define BOOST_LIB_NAME boost_serialization
//
// And include the header that does the work:
//
#include <boost/config/auto_link.hpp>

#endif  // !defined(BOOST_SERIALIZATION_SOURCE) && !defined(BOOST_ARCHIVE_SOURCE)

//----------------------------------------------------------------------------// 

#endif // BOOST_SERIALIZATION_CONFIG_HPP
