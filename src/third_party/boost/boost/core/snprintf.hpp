/*
 *             Copyright Andrey Semashev 2022.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   snprintf.hpp
 * \author Andrey Semashev
 * \date   06.12.2022
 *
 * \brief  The header provides more portable definition of snprintf and vsnprintf,
 *         as well as \c wchar_t counterparts.
 */

#ifndef BOOST_CORE_SNPRINTF_HPP_INCLUDED_
#define BOOST_CORE_SNPRINTF_HPP_INCLUDED_

#include <stdio.h>
#include <wchar.h>
#include <boost/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if defined(__MINGW32__)

#include <cstddef>
#include <cstdarg>
#if !defined(__MINGW64_VERSION_MAJOR)
#include <climits>
#endif

// MinGW32 and MinGW-w64 provide their own snprintf implementations that are compliant with the C standard.
#define BOOST_CORE_DETAIL_MINGW_SNPRINTF

#elif (defined(BOOST_MSSTL_VERSION) && BOOST_MSSTL_VERSION < 140)

#include <cstddef>
#include <cstdarg>
#include <climits>

// MSVC snprintfs are not conforming but they are good enough for typical use cases.
#define BOOST_CORE_DETAIL_MSVC_LEGACY_SNPRINTF

#endif

namespace boost {

namespace core {

#if defined(BOOST_CORE_DETAIL_MINGW_SNPRINTF) || defined(BOOST_CORE_DETAIL_MSVC_LEGACY_SNPRINTF)

#if defined(BOOST_CORE_DETAIL_MINGW_SNPRINTF)

inline int vsnprintf(char* buf, std::size_t size, const char* format, std::va_list args)
{
    return __mingw_vsnprintf(buf, size, format, args);
}

inline int vswprintf(wchar_t* buf, std::size_t size, const wchar_t* format, std::va_list args)
{
#if defined(__MINGW64_VERSION_MAJOR)
    int res = __mingw_vsnwprintf(buf, size, format, args);
    // __mingw_vsnwprintf returns the number of characters to be printed, but (v)swprintf is expected to return -1 on truncation
    if (static_cast< unsigned int >(res) >= size)
        res = -1;
    return res;
#else
    // Legacy MinGW32 does not provide __mingw_vsnwprintf, so use _vsnwprintf from MSVC CRT
    if (BOOST_UNLIKELY(size == 0u || size > static_cast< std::size_t >(INT_MAX)))
        return -1;

    int res = _vsnwprintf(buf, size, format, args);
    // (v)swprintf is expected to return -1 on truncation, so we only need to ensure the output is null-terminated
    if (static_cast< unsigned int >(res) >= size)
    {
        buf[size - 1u] = L'\0';
        res = -1;
    }

    return res;
#endif
}

#elif defined(BOOST_CORE_DETAIL_MSVC_LEGACY_SNPRINTF)

#if defined(_MSC_VER)
#pragma warning(push)
// '_vsnprintf': This function or variable may be unsafe. Consider using _vsnprintf_s instead.
#pragma warning(disable: 4996)
#endif

inline int vsnprintf(char* buf, std::size_t size, const char* format, std::va_list args)
{
    if (BOOST_UNLIKELY(size == 0u))
        return 0;
    if (BOOST_UNLIKELY(size > static_cast< std::size_t >(INT_MAX)))
        return -1;

    buf[size - 1u] = '\0';
    int res = _vsnprintf(buf, size, format, args);
    if (static_cast< unsigned int >(res) >= size)
    {
        // _vsnprintf returns -1 if the output was truncated and in case of other errors.
        // Detect truncation by checking whether the output buffer was written over entirely.
        if (buf[size - 1u] != '\0')
        {
            buf[size - 1u] = '\0';
            res = static_cast< int >(size);
        }
    }

    return res;
}

inline int vswprintf(wchar_t* buf, std::size_t size, const wchar_t* format, std::va_list args)
{
    if (BOOST_UNLIKELY(size == 0u || size > static_cast< std::size_t >(INT_MAX)))
        return -1;

    int res = _vsnwprintf(buf, size, format, args);
    // (v)swprintf is expected to return -1 on truncation, so we only need to ensure the output is null-terminated
    if (static_cast< unsigned int >(res) >= size)
    {
        buf[size - 1u] = L'\0';
        res = -1;
    }

    return res;
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif

inline int snprintf(char* buf, std::size_t size, const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    int res = vsnprintf(buf, size, format, args);
    va_end(args);
    return res;
}

inline int swprintf(wchar_t* buf, std::size_t size, const wchar_t* format, ...)
{
    std::va_list args;
    va_start(args, format);
    int res = vswprintf(buf, size, format, args);
    va_end(args);
    return res;
}

#else // defined(BOOST_CORE_DETAIL_MINGW_SNPRINTF) || defined(BOOST_CORE_DETAIL_MSVC_LEGACY_SNPRINTF)

// Standard-conforming compilers already have the correct snprintfs
using ::snprintf;
using ::vsnprintf;

using ::swprintf;
using ::vswprintf;

#endif // defined(BOOST_CORE_DETAIL_MINGW_SNPRINTF) || defined(BOOST_CORE_DETAIL_MSVC_LEGACY_SNPRINTF)

} // namespace core

} // namespace boost

#endif // BOOST_CORE_SNPRINTF_HPP_INCLUDED_
