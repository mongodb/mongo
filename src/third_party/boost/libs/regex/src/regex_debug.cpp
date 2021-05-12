/*
 *
 * Copyright (c) 1998-2004
 * John Maddock
 *
 * Use, modification and distribution are subject to the 
 * Boost Software License, Version 1.0. (See accompanying file 
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 */

 /*
  *   LOCATION:    see http://www.boost.org for most recent version.
  *   FILE:        regex_debug.cpp
  *   VERSION:     see <boost/version.hpp>
  *   DESCRIPTION: Misc. debugging helpers.
  */


#define BOOST_REGEX_SOURCE

#include <boost/regex/config.hpp>


//
// regex configuration information: this prints out the settings used
// when the library was built - include in debugging builds only:
//
#ifdef BOOST_REGEX_CONFIG_INFO

#define print_macro regex_lib_print_macro
#define print_expression regex_lib_print_expression
#define print_byte_order regex_lib_print_byte_order
#define print_sign regex_lib_print_sign
#define print_compiler_macros regex_lib_print_compiler_macros
#define print_stdlib_macros regex_lib_print_stdlib_macros
#define print_platform_macros regex_lib_print_platform_macros
#define print_boost_macros regex_lib_print_boost_macros
#define print_separator regex_lib_print_separator
#define OLD_MAIN regex_lib_main
#define NEW_MAIN regex_lib_main2
#define NO_RECURSE

#include <libs/regex/test/config_info/regex_config_info.cpp>

BOOST_REGEX_DECL void BOOST_REGEX_CALL print_regex_library_info()
{
   std::cout << "\n\n";
   print_separator();
   std::cout << "Regex library build configuration:\n\n";
   regex_lib_main2();
}

#endif





