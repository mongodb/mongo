/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */

#if !defined(BOOST_SCOPE_ENABLE_WARNINGS)

#if defined(_MSC_VER) && !defined(__clang__)

#pragma warning(pop)

#elif (defined(__GNUC__) && !(defined(__INTEL_COMPILER) || defined(__ICL) || defined(__ICC) || defined(__ECC)) \
    && (__GNUC__ * 100 + __GNUC_MINOR__) >= 406) || defined(__clang__)

#pragma GCC diagnostic pop

#endif

#endif // !defined(BOOST_SCOPE_ENABLE_WARNINGS)
