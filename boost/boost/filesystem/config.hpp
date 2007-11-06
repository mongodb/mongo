//  boost/filesystem/config.hpp  ---------------------------------------------//

//  Copyright Beman Dawes 2003
//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

//----------------------------------------------------------------------------// 

#ifndef BOOST_FILESYSTEM_CONFIG_HPP
#define BOOST_FILESYSTEM_CONFIG_HPP

#define BOOST_FILESYSTEM_I18N  // aid users wishing to compile several versions

//  ability to change namespace aids path_table.cpp  ------------------------// 
#ifndef BOOST_FILESYSTEM_NAMESPACE
# define BOOST_FILESYSTEM_NAMESPACE filesystem
#endif

// This header implements separate compilation features as described in
// http://www.boost.org/more/separate_compilation.html

#include <boost/config.hpp>

//  determine platform  ------------------------------------------------------//

//  BOOST_CYGWIN_PATH implies BOOST_WINDOWS_PATH and BOOST_POSIX_API

# if defined(BOOST_CYGWIN_PATH)
#   if defined(BOOST_POSIX_PATH)
#     error BOOST_POSIX_PATH is invalid when BOOST_CYGWIN_PATH is defined
#   endif
#   if defined(BOOST_WINDOWS_API)
#     error BOOST_WINDOWS_API is invalid when BOOST_CYGWIN_PATH is defined
#   endif
#   define BOOST_WINDOWS_PATH
#   define BOOST_POSIX_API
# endif

//  BOOST_POSIX_API or BOOST_WINDOWS_API specify which API to use

# if defined( BOOST_WINDOWS_API ) && defined( BOOST_POSIX_API )
#   error both BOOST_WINDOWS_API and BOOST_POSIX_API are defined
# elif !defined( BOOST_WINDOWS_API ) && !defined( BOOST_POSIX_API )
#   if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(__CYGWIN__)
#     define BOOST_WINDOWS_API
#   else
#     define BOOST_POSIX_API 
#   endif
# endif

//  BOOST_WINDOWS_PATH enables Windows path syntax recognition

# if !defined(BOOST_POSIX_PATH) && (defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(__CYGWIN__))
#   define BOOST_WINDOWS_PATH
# endif

//  narrow support only for badly broken compilers or libraries  -------------//

# if defined(BOOST_NO_STD_WSTRING) || defined(BOOST_NO_SFINAE) || defined(BOOST_NO_STD_LOCALE)
#   define BOOST_FILESYSTEM_NARROW_ONLY
# endif

//  enable dynamic linking on Windows  ---------------------------------------//

#  if (defined(BOOST_ALL_DYN_LINK) || defined(BOOST_FILESYSTEM_DYN_LINK)) && defined(__BORLANDC__) && defined(__WIN32__)
#    error Dynamic linking Boost.Filesystem does not work for Borland; use static linking instead
#  endif

#ifdef BOOST_HAS_DECLSPEC // defined in config system
// we need to import/export our code only if the user has specifically
// asked for it by defining either BOOST_ALL_DYN_LINK if they want all boost
// libraries to be dynamically linked, or BOOST_FILESYSTEM_DYN_LINK
// if they want just this one to be dynamically liked:
#if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_FILESYSTEM_DYN_LINK)
// export if this is our own source, otherwise import:
#ifdef BOOST_FILESYSTEM_SOURCE
# define BOOST_FILESYSTEM_DECL __declspec(dllexport)
#else
# define BOOST_FILESYSTEM_DECL __declspec(dllimport)
#endif  // BOOST_FILESYSTEM_SOURCE
#endif  // DYN_LINK
#endif  // BOOST_HAS_DECLSPEC
//
// if BOOST_FILESYSTEM_DECL isn't defined yet define it now:
#ifndef BOOST_FILESYSTEM_DECL
#define BOOST_FILESYSTEM_DECL
#endif

//  enable automatic library variant selection  ------------------------------// 

#if !defined(BOOST_FILESYSTEM_SOURCE) && !defined(BOOST_ALL_NO_LIB) && !defined(BOOST_FILESYSTEM_NO_LIB)
//
// Set the name of our library, this will get undef'ed by auto_link.hpp
// once it's done with it:
//
#define BOOST_LIB_NAME boost_filesystem
//
// If we're importing code from a dll, then tell auto_link.hpp about it:
//
#if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_FILESYSTEM_DYN_LINK)
#  define BOOST_DYN_LINK
#endif
//
// And include the header that does the work:
//
#include <boost/config/auto_link.hpp>
#endif  // auto-linking disabled

#endif // BOOST_FILESYSTEM_CONFIG_HPP
