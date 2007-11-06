// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef WORKAROUND_DWA2002126_HPP
# define WORKAROUND_DWA2002126_HPP

// Compiler/library version workaround macro
//
// Usage:
//
//     #if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
//        // workaround for eVC4 and VC6
//        ... // workaround code here
//     #endif
//
// When BOOST_STRICT_CONFIG is defined, expands to 0. Otherwise, the
// first argument must be undefined or expand to a numeric
// value. The above expands to:
//
//     (BOOST_MSVC) != 0 && (BOOST_MSVC) < 1300
//
// When used for workarounds that apply to the latest known version 
// and all earlier versions of a compiler, the following convention 
// should be observed:
//
//     #if BOOST_WORKAROUND(BOOST_MSVC, BOOST_TESTED_AT(1301))
//
// The version number in this case corresponds to the last version in
// which the workaround was known to have been required. When
// BOOST_DETECT_OUTDATED_WORKAROUNDS is not the defined, the macro
// BOOST_TESTED_AT(x) expands to "!= 0", which effectively activates
// the workaround for any version of the compiler. When
// BOOST_DETECT_OUTDATED_WORKAROUNDS is defined, a compiler warning or
// error will be issued if the compiler version exceeds the argument
// to BOOST_TESTED_AT().  This can be used to locate workarounds which
// may be obsoleted by newer versions.

# ifndef BOOST_STRICT_CONFIG

#  define BOOST_WORKAROUND(symbol, test)                \
        ((symbol != 0) && (1 % (( (symbol test) ) + 1)))
//                              ^ ^           ^ ^
// The extra level of parenthesis nesting above, along with the
// BOOST_OPEN_PAREN indirection below, is required to satisfy the
// broken preprocessor in MWCW 8.3 and earlier.
//
// The basic mechanism works as follows:
//      (symbol test) + 1        =>   if (symbol test) then 2 else 1
//      1 % ((symbol test) + 1)  =>   if (symbol test) then 1 else 0
//
// The complication with % is for cooperation with BOOST_TESTED_AT().
// When "test" is BOOST_TESTED_AT(x) and
// BOOST_DETECT_OUTDATED_WORKAROUNDS is #defined,
//
//      symbol test              =>   if (symbol <= x) then 1 else -1
//      (symbol test) + 1        =>   if (symbol <= x) then 2 else 0
//      1 % ((symbol test) + 1)  =>   if (symbol <= x) then 1 else divide-by-zero
//

#  ifdef BOOST_DETECT_OUTDATED_WORKAROUNDS
#   define BOOST_OPEN_PAREN (
#   define BOOST_TESTED_AT(value)  > value) ?(-1): BOOST_OPEN_PAREN 1
#  else
#   define BOOST_TESTED_AT(value) != ((value)-(value))
#  endif

# else

#  define BOOST_WORKAROUND(symbol, test) 0

# endif 

#endif // WORKAROUND_DWA2002126_HPP
