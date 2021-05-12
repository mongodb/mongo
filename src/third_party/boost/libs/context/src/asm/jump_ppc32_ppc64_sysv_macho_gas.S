/*
            Copyright Sergue E. Leontiev 2013.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

// Stub file for universal binary

#if defined(__ppc__)
    #include "jump_ppc32_sysv_macho_gas.S"
#elif defined(__ppc64__)
    #include "jump_ppc64_sysv_macho_gas.S"
#else
    #error "No arch's"
#endif
