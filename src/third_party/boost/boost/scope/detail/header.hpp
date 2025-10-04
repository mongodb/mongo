/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */

#if !defined(BOOST_SCOPE_ENABLE_WARNINGS)

#if defined(_MSC_VER) && !defined(__clang__)

#pragma warning(push, 3)
// unreferenced formal parameter
#pragma warning(disable: 4100)
// conditional expression is constant
#pragma warning(disable: 4127)
// function marked as __forceinline not inlined
#pragma warning(disable: 4714)
// decorated name length exceeded, name was truncated
#pragma warning(disable: 4503)
// qualifier applied to function type has no meaning; ignored
#pragma warning(disable: 4180)
// qualifier applied to reference type; ignored
#pragma warning(disable: 4181)
// unreachable code
#pragma warning(disable: 4702)
// destructor never returns, potential memory leak
#pragma warning(disable: 4722)

#elif (defined(__GNUC__) && !(defined(__INTEL_COMPILER) || defined(__ICL) || defined(__ICC) || defined(__ECC)) \
    && (__GNUC__ * 100 + __GNUC_MINOR__) >= 406) || defined(__clang__)

// Note: clang-cl goes here as well, as it seems to support gcc-style warning control pragmas.

#pragma GCC diagnostic push
// unused parameter 'arg'
#pragma GCC diagnostic ignored "-Wunused-parameter"
// unused function 'foo'
#pragma GCC diagnostic ignored "-Wunused-function"

#if defined(__clang__)
// template argument uses unnamed type
#pragma clang diagnostic ignored "-Wunnamed-type-template-args"
#endif // defined(__clang__)

#endif

#endif // !defined(BOOST_SCOPE_ENABLE_WARNINGS)
