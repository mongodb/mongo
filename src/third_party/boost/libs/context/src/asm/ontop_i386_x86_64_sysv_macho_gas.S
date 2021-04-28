/*
            Copyright Sergue E. Leontiev 2013.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

// Stub file for universal binary

#if defined(__i386__)
    #include "ontop_i386_sysv_macho_gas.S"
#elif defined(__x86_64__)
    #include "ontop_x86_64_sysv_macho_gas.S"
#else
    #error "No arch's"
#endif
