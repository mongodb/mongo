#ifndef BOOST_STATIC_WARNING_HPP
#define BOOST_STATIC_WARNING_HPP

//  (C) Copyright Robert Ramey 2003. Jonathan Turkanis 2004.
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/static_assert for documentation.

/*
 Revision history:
   15 June  2003 - Initial version.
   31 March 2004 - improved diagnostic messages and portability 
                   (Jonathan Turkanis)
   03 April 2004 - works on VC6 at class and namespace scope
                 - ported to DigitalMars
                 - static warnings disabled by default; when enabled,
                   uses pragmas to enable required compiler warnings
                   on MSVC, Intel, Metrowerks and Borland 5.x.
                   (Jonathan Turkanis)
   30 May 2004   - tweaked for msvc 7.1 and gcc 3.3
                 - static warnings ENabled by default; when enabled,
                   (Robert Ramey)
*/

#include <boost/config.hpp>

//
// Implementation
// Makes use of the following warnings:
//  1. GCC prior to 3.3: division by zero.
//  2. BCC 6.0 preview: unreferenced local variable.
//  3. DigitalMars: returning address of local automatic variable.
//  4. VC6: class previously seen as struct (as in 'boost/mpl/print.hpp')
//  5. All others: deletion of pointer to incomplete type.
//
// The trick is to find code which produces warnings containing the name of
// a structure or variable. Details, with same numbering as above:
// 1. static_warning_impl<B>::value is zero iff B is false, so diving an int
//    by this value generates a warning iff B is false.
// 2. static_warning_impl<B>::type has a constructor iff B is true, so an
//    unreferenced variable of this type generates a warning iff B is false.
// 3. static_warning_impl<B>::type overloads operator& to return a dynamically
//    allocated int pointer only is B is true, so  returning the address of an
//    automatic variable of this type generates a warning iff B is fasle.
// 4. static_warning_impl<B>::STATIC_WARNING is decalred as a struct iff B is 
//    false. 
// 5. static_warning_impl<B>::type is incomplete iff B is false, so deleting a
//    pointer to this type generates a warning iff B is false.
//

//------------------Enable selected warnings----------------------------------//

// Enable the warnings relied on by BOOST_STATIC_WARNING, where possible. The 
// only pragma which is absolutely necessary here is for Borland 5.x, since 
// W8073 is disabled by default. If enabling selected warnings is considered 
// unacceptable, this section can be replaced with:
//   #if defined(__BORLANDC__) && (__BORLANDC__ <= 0x600)
//    pragma warn +stu
//   #endif

# if defined(BOOST_MSVC)
#  pragma warning(2:4150) // C4150: deletion of pointer to incomplete type 'type'.
# elif defined(BOOST_INTEL) && (defined(__WIN32__) || defined(WIN32))
#  pragma warning(2:457) // #457: delete of pointer to incomplete class.
# elif defined(__BORLANDC__) && (__BORLANDC__ <= 0x600)
#  pragma warn +stu  // W8073: Undefined structure 'structure'.
# elif defined(__MWERKS__)
#  pragma extended_errorcheck on // Enable 'extended error checking'.
# endif

//------------------Configure-------------------------------------------------//

# if defined(__BORLANDC__) && (__BORLANDC__ >= 0x600)
#  define BOOST_HAS_DESCRIPTIVE_UNREFERENCED_VARIABLE_WARNING
# elif defined(__GNUC__) && !defined(BOOST_INTEL) // && (__GNUC__ * 100 + __GNUC_MINOR__ <= 302)
#  define BOOST_HAS_DESCRIPTIVE_DIVIDE_BY_ZERO_WARNING
# elif defined(__DMC__)
#  define BOOST_HAS_DESCRIPTIVE_RETURNING_ADDRESS_OF_TEMPORARY_WARNING
# elif defined(BOOST_MSVC) // && (BOOST_MSVC < 1300)
#  define BOOST_NO_PREDEFINED_LINE_MACRO
#  pragma warning(disable:4094) // C4094: untagged 'stuct' declared no symbols
#endif

//------------------Helper templates------------------------------------------//

namespace boost {

struct STATIC_WARNING;

template<bool>
struct static_warning_impl;

template<>
struct static_warning_impl<false> {
    enum { value = 0 };
    #if !defined(BOOST_HAS_DESCRIPTIVE_UNREFERENCED_VARIABLE_WARNING) && \
        !defined(BOOST_HAS_DESCRIPTIVE_RETURNING_ADDRESS_OF_TEMPORARY_WARNING)
        typedef boost::STATIC_WARNING type;
    #else
        typedef int type;
    #endif
    #if defined(BOOST_NO_PREDEFINED_LINE_MACRO)
        struct STATIC_WARNING { };
    #endif
};

template<>
struct static_warning_impl<true> {
    enum { value = 1 };
    struct type { type() { } int* operator&() { return new int; } };
    #if defined(BOOST_NO_PREDEFINED_LINE_MACRO)
        class STATIC_WARNING { };
    #endif
};

} // namespace boost

//------------------Definition of BOOST_STATIC_WARNING------------------------//

#if defined(BOOST_HAS_DESCRIPTIVE_UNREFERENCED_VARIABLE_WARNING)
#    define BOOST_STATIC_WARNING_IMPL(B)                   \
     struct BOOST_JOIN(STATIC_WARNING, __LINE__) {         \
       void f() {                                          \
           ::boost::static_warning_impl<(bool)( B )>::type \
           STATIC_WARNING;                                 \
       }                                                   \
     }                                                     \
     /**/
#elif defined(BOOST_HAS_DESCRIPTIVE_RETURNING_ADDRESS_OF_TEMPORARY_WARNING)
#    define BOOST_STATIC_WARNING_IMPL(B)                        \
     struct BOOST_JOIN(STATIC_WARNING, __LINE__) {              \
        int* f() {                                              \
            ::boost::static_warning_impl<(bool)( B )>::type     \
            STATIC_WARNING;                                     \
            return &STATIC_WARNING;                             \
        }                                                       \
     }                                                          \
     /**/
#elif defined(BOOST_HAS_DESCRIPTIVE_DIVIDE_BY_ZERO_WARNING)
#    define BOOST_STATIC_WARNING_IMPL(B)                             \
     struct BOOST_JOIN(STATIC_WARNING, __LINE__) {                   \
         int f() { int STATIC_WARNING = 1;                           \
                   return STATIC_WARNING /                           \
                   boost::static_warning_impl<(bool)( B )>::value; } \
     }                                                               \
     /**/
#elif defined(BOOST_NO_PREDEFINED_LINE_MACRO) 
     // VC6; __LINE__ macro broken when -ZI is used see Q199057, so 
     // non-conforming workaround is used.
#    define BOOST_STATIC_WARNING_IMPL(B)                       \
     struct {                                                  \
        struct S {                                             \
            typedef boost::static_warning_impl<(bool)( B )> f; \
            friend class f::STATIC_WARNING;                    \
        };                                                     \
     }                                                         \
     /**/
#else // Deletion of pointer to incomplete type.
#    define BOOST_STATIC_WARNING_IMPL(B)                     \
     struct BOOST_JOIN(STATIC_WARNING, __LINE__) {           \
         ::boost::static_warning_impl<(bool)( B )>::type* p; \
         void f() { delete p; }                              \
     }                                                       \
     /**/
#endif

#ifndef BOOST_DISABLE_STATIC_WARNINGS
# define BOOST_STATIC_WARNING(B) BOOST_STATIC_WARNING_IMPL(B)
#else // #ifdef BOOST_ENABLE_STATIC_WARNINGS //-------------------------------//
# define BOOST_STATIC_WARNING(B) BOOST_STATIC_WARNING_IMPL(true)
#endif

#endif // BOOST_STATIC_WARNING_HPP
