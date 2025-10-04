//  private_config.hpp  ----------------------------------------------------------------//

//  Copyright 2021 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_SRC_PRIVATE_CONFIG_HPP_
#define BOOST_FILESYSTEM_SRC_PRIVATE_CONFIG_HPP_

#include <boost/filesystem/config.hpp>

#if defined(BOOST_FILESYSTEM_HAS_INIT_PRIORITY)
#define BOOST_FILESYSTEM_INIT_PRIORITY(n) __attribute__ ((init_priority(n)))
#else
#define BOOST_FILESYSTEM_INIT_PRIORITY(n)
#endif

// According to https://gcc.gnu.org/bugzilla//show_bug.cgi?id=65115,
// the default C++ object initializers priority is 65535. We would like to
// initialize function pointers earlier than that (with lower priority values),
// before the other global objects initializers are run. Other than this,
// these priority values are arbitrary.
#define BOOST_FILESYSTEM_FUNC_PTR_INIT_PRIORITY 32767

// Path globals initialization priority
#define BOOST_FILESYSTEM_PATH_GLOBALS_INIT_PRIORITY 32768

#if defined(__has_feature) && defined(__has_attribute)
#if __has_feature(memory_sanitizer) && __has_attribute(no_sanitize)
#define BOOST_FILESYSTEM_NO_SANITIZE_MEMORY __attribute__ ((no_sanitize("memory")))
#endif
#endif // defined(__has_feature) && defined(__has_attribute)

#ifndef BOOST_FILESYSTEM_NO_SANITIZE_MEMORY
#define BOOST_FILESYSTEM_NO_SANITIZE_MEMORY
#endif

#if defined(_MSC_VER)
#if _MSC_VER < 1300 || _MSC_VER > 1900 // 1300 == VC++ 7.0, 1900 == VC++ 14.0
typedef void (__cdecl* init_func_ptr_t)();
#define BOOST_FILESYSTEM_INITRETSUCCESS_V
#define BOOST_FILESYSTEM_INIT_FUNC void __cdecl
#else
typedef int (__cdecl* init_func_ptr_t)();
#define BOOST_FILESYSTEM_INITRETSUCCESS_V 0
#define BOOST_FILESYSTEM_INIT_FUNC int __cdecl
#endif
#else // defined(_MSC_VER)
typedef void (*init_func_ptr_t)();
#define BOOST_FILESYSTEM_INITRETSUCCESS_V
#define BOOST_FILESYSTEM_INIT_FUNC void
#endif // defined(_MSC_VER)

#if defined(__has_attribute)
#if __has_attribute(__used__)
#define BOOST_FILESYSTEM_ATTRIBUTE_RETAIN __attribute__ ((__used__))
#endif
#endif

#if !defined(BOOST_FILESYSTEM_ATTRIBUTE_RETAIN) && defined(__GNUC__) && (__GNUC__ * 100 + __GNUC_MINOR__) >= 402
#define BOOST_FILESYSTEM_ATTRIBUTE_RETAIN __attribute__ ((__used__))
#endif

#if !defined(BOOST_FILESYSTEM_ATTRIBUTE_RETAIN)
#define BOOST_FILESYSTEM_NO_ATTRIBUTE_RETAIN
#define BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
#endif

#endif // BOOST_FILESYSTEM_SRC_PRIVATE_CONFIG_HPP_
