//
// Copyright (c) 2023-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_UTIL_FOREACH_CHAR_HPP
#define BOOST_LOCALE_UTIL_FOREACH_CHAR_HPP

#include <boost/locale/config.hpp>

#ifdef __cpp_lib_char8_t
#    define BOOST_LOCALE_FOREACH_CHAR_I_CHAR8_T(F) F(char8_t)
#    define BOOST_LOCALE_FOREACH_CHAR_I2_CHAR8_T(F) F(char8_t)
#elif defined(__cpp_char8_t)
#    define BOOST_LOCALE_FOREACH_CHAR_I_CHAR8_T(F) F(char8_t)
#    define BOOST_LOCALE_FOREACH_CHAR_I2_CHAR8_T(F)
#else
#    define BOOST_LOCALE_FOREACH_CHAR_I_CHAR8_T(F)
#    define BOOST_LOCALE_FOREACH_CHAR_I2_CHAR8_T(F)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
#    define BOOST_LOCALE_FOREACH_CHAR_I_CHAR16_T(F) F(char16_t)
#else
#    define BOOST_LOCALE_FOREACH_CHAR_I_CHAR16_T(F)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
#    define BOOST_LOCALE_FOREACH_CHAR_I_CHAR32_T(F) F(char32_t)
#else
#    define BOOST_LOCALE_FOREACH_CHAR_I_CHAR32_T(F)
#endif

#define BOOST_LOCALE_FOREACH_CHAR(F)        \
    F(char)                                 \
    F(wchar_t)                              \
    BOOST_LOCALE_FOREACH_CHAR_I_CHAR8_T(F)  \
    BOOST_LOCALE_FOREACH_CHAR_I_CHAR16_T(F) \
    BOOST_LOCALE_FOREACH_CHAR_I_CHAR32_T(F)

// Same but only where std::basic_string<C> is available
#define BOOST_LOCALE_FOREACH_CHAR_STRING(F) \
    F(char)                                 \
    F(wchar_t)                              \
    BOOST_LOCALE_FOREACH_CHAR_I2_CHAR8_T(F) \
    BOOST_LOCALE_FOREACH_CHAR_I_CHAR16_T(F) \
    BOOST_LOCALE_FOREACH_CHAR_I_CHAR32_T(F)

#endif
