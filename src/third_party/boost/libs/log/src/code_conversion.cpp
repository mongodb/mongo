/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   code_conversion.cpp
 * \author Andrey Semashev
 * \date   08.11.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <locale>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <boost/log/exceptions.hpp>
#include <boost/log/detail/code_conversion.hpp>
#if defined(BOOST_WINDOWS)
#include <cstring>
#include <limits>
#include <boost/winapi/get_last_error.hpp>
#include <boost/winapi/character_code_conversion.hpp>
#endif
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! The function performs character conversion with the specified facet
template< typename LocalCharT >
inline std::codecvt_base::result convert(
    std::codecvt< LocalCharT, char, std::mbstate_t > const& fac,
    std::mbstate_t& state,
    const char*& pSrcBegin,
    const char* pSrcEnd,
    LocalCharT*& pDstBegin,
    LocalCharT* pDstEnd)
{
    return fac.in(state, pSrcBegin, pSrcEnd, pSrcBegin, pDstBegin, pDstEnd, pDstBegin);
}

//! The function performs character conversion with the specified facet
template< typename LocalCharT >
inline std::codecvt_base::result convert(
    std::codecvt< LocalCharT, char, std::mbstate_t > const& fac,
    std::mbstate_t& state,
    const LocalCharT*& pSrcBegin,
    const LocalCharT* pSrcEnd,
    char*& pDstBegin,
    char* pDstEnd)
{
    return fac.out(state, pSrcBegin, pSrcEnd, pSrcBegin, pDstBegin, pDstEnd, pDstBegin);
}

} // namespace

template< typename SourceCharT, typename TargetCharT, typename FacetT >
inline std::size_t code_convert(const SourceCharT* begin, const SourceCharT* end, std::basic_string< TargetCharT >& converted, std::size_t max_size, FacetT const& fac)
{
    typedef typename FacetT::state_type state_type;
    TargetCharT converted_buffer[256];

    const SourceCharT* const original_begin = begin;
    state_type state = state_type();
    std::size_t buf_size = (std::min)(max_size, sizeof(converted_buffer) / sizeof(*converted_buffer));
    while (begin != end && buf_size > 0u)
    {
        TargetCharT* dest = converted_buffer;
        std::codecvt_base::result res = convert(
            fac,
            state,
            begin,
            end,
            dest,
            dest + buf_size);

        switch (res)
        {
        case std::codecvt_base::ok:
            // All characters were successfully converted
            // NOTE: MSVC 11 also returns ok when the source buffer was only partially consumed, so we also check that the begin pointer has reached the end.
            converted.append(converted_buffer, dest);
            max_size -= dest - converted_buffer;
            break;

        case std::codecvt_base::noconv:
            {
                // Not possible, unless both character types are actually equivalent
                const std::size_t size = (std::min)(max_size, static_cast< std::size_t >(end - begin));
                converted.append(begin, begin + size);
                begin += size;
                max_size -= size;
            }
            goto done;

        case std::codecvt_base::partial:
            // Some characters were converted, some were not
            if (dest != converted_buffer)
            {
                // Some conversion took place, so it seems like
                // the destination buffer might not have been long enough
                converted.append(converted_buffer, dest);
                max_size -= dest - converted_buffer;

                // ...and go on for the next part
                break;
            }
            else
            {
                // Nothing was converted
                if (begin == end)
                    goto done;

                // Looks like the tail of the source buffer contains only part of the last character.
                // In this case we intentionally fall through to throw an exception.
            }
            BOOST_FALLTHROUGH;

        default: // std::codecvt_base::error
            BOOST_LOG_THROW_DESCR(conversion_error, "Could not convert character encoding");
        }

        buf_size = (std::min)(max_size, sizeof(converted_buffer) / sizeof(*converted_buffer));
    }

done:
    return static_cast< std::size_t >(begin - original_begin);
}

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const wchar_t* str1, std::size_t len, std::string& str2, std::size_t max_size, std::locale const& loc)
{
    return code_convert(str1, str1 + len, str2, max_size, std::use_facet< std::codecvt< wchar_t, char, std::mbstate_t > >(loc)) == len;
}

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char* str1, std::size_t len, std::wstring& str2, std::size_t max_size, std::locale const& loc)
{
    return code_convert(str1, str1 + len, str2, max_size, std::use_facet< std::codecvt< wchar_t, char, std::mbstate_t > >(loc)) == len;
}

#if !defined(BOOST_LOG_NO_CXX11_CODECVT_FACETS)

#if !defined(BOOST_NO_CXX11_CHAR16_T)

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char16_t* str1, std::size_t len, std::string& str2, std::size_t max_size, std::locale const& loc)
{
    return code_convert(str1, str1 + len, str2, max_size, std::use_facet< std::codecvt< char16_t, char, std::mbstate_t > >(loc)) == len;
}

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char* str1, std::size_t len, std::u16string& str2, std::size_t max_size, std::locale const& loc)
{
    return code_convert(str1, str1 + len, str2, max_size, std::use_facet< std::codecvt< char16_t, char, std::mbstate_t > >(loc)) == len;
}

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char16_t* str1, std::size_t len, std::wstring& str2, std::size_t max_size, std::locale const& loc)
{
    std::string temp_str;
    code_convert(str1, str1 + len, temp_str, temp_str.max_size(), std::use_facet< std::codecvt< char16_t, char, std::mbstate_t > >(loc));
    const std::size_t temp_size = temp_str.size();
    return code_convert(temp_str.c_str(), temp_str.c_str() + temp_size, str2, max_size, std::use_facet< std::codecvt< wchar_t, char, std::mbstate_t > >(loc)) == temp_size;
}

#endif

#if !defined(BOOST_NO_CXX11_CHAR32_T)

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char32_t* str1, std::size_t len, std::string& str2, std::size_t max_size, std::locale const& loc)
{
    return code_convert(str1, str1 + len, str2, max_size, std::use_facet< std::codecvt< char32_t, char, std::mbstate_t > >(loc)) == len;
}

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char* str1, std::size_t len, std::u32string& str2, std::size_t max_size, std::locale const& loc)
{
    return code_convert(str1, str1 + len, str2, max_size, std::use_facet< std::codecvt< char32_t, char, std::mbstate_t > >(loc)) == len;
}

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char32_t* str1, std::size_t len, std::wstring& str2, std::size_t max_size, std::locale const& loc)
{
    std::string temp_str;
    code_convert(str1, str1 + len, temp_str, temp_str.max_size(), std::use_facet< std::codecvt< char32_t, char, std::mbstate_t > >(loc));
    const std::size_t temp_size = temp_str.size();
    return code_convert(temp_str.c_str(), temp_str.c_str() + temp_size, str2, max_size, std::use_facet< std::codecvt< wchar_t, char, std::mbstate_t > >(loc)) == temp_size;
}

#endif

#if !defined(BOOST_NO_CXX11_CHAR16_T) && !defined(BOOST_NO_CXX11_CHAR32_T)

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char16_t* str1, std::size_t len, std::u32string& str2, std::size_t max_size, std::locale const& loc)
{
    std::string temp_str;
    code_convert(str1, str1 + len, temp_str, temp_str.max_size(), std::use_facet< std::codecvt< char16_t, char, std::mbstate_t > >(loc));
    const std::size_t temp_size = temp_str.size();
    return code_convert(temp_str.c_str(), temp_str.c_str() + temp_size, str2, max_size, std::use_facet< std::codecvt< char32_t, char, std::mbstate_t > >(loc)) == temp_size;
}

//! The function converts one string to the character type of another
BOOST_LOG_API bool code_convert_impl(const char32_t* str1, std::size_t len, std::u16string& str2, std::size_t max_size, std::locale const& loc)
{
    std::string temp_str;
    code_convert(str1, str1 + len, temp_str, temp_str.max_size(), std::use_facet< std::codecvt< char32_t, char, std::mbstate_t > >(loc));
    const std::size_t temp_size = temp_str.size();
    return code_convert(temp_str.c_str(), temp_str.c_str() + temp_size, str2, max_size, std::use_facet< std::codecvt< char16_t, char, std::mbstate_t > >(loc)) == temp_size;
}

#endif

#endif // !defined(BOOST_LOG_NO_CXX11_CODECVT_FACETS)

#if defined(BOOST_WINDOWS)

//! Converts UTF-8 to UTF-16
std::wstring utf8_to_utf16(const char* str)
{
    std::size_t utf8_len = std::strlen(str);
    if (utf8_len == 0)
        return std::wstring();
    else if (BOOST_UNLIKELY(utf8_len > static_cast< std::size_t >((std::numeric_limits< int >::max)())))
        BOOST_LOG_THROW_DESCR(bad_alloc, "UTF-8 string too long");

    int len = boost::winapi::MultiByteToWideChar(boost::winapi::CP_UTF8_, boost::winapi::MB_ERR_INVALID_CHARS_, str, static_cast< int >(utf8_len), NULL, 0);
    if (BOOST_LIKELY(len > 0))
    {
        std::wstring wstr;
        wstr.resize(len);

        len = boost::winapi::MultiByteToWideChar(boost::winapi::CP_UTF8_, boost::winapi::MB_ERR_INVALID_CHARS_, str, static_cast< int >(utf8_len), &wstr[0], len);
        if (BOOST_LIKELY(len > 0))
        {
            return wstr;
        }
    }

    const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
    BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to convert UTF-8 to UTF-16", (err));
    BOOST_LOG_UNREACHABLE_RETURN(std::wstring());
}

//! Converts UTF-16 to UTF-8
std::string utf16_to_utf8(const wchar_t* wstr)
{
    std::size_t utf16_len = std::wcslen(wstr);
    if (utf16_len == 0)
        return std::string();
    else if (BOOST_UNLIKELY(utf16_len > static_cast< std::size_t >((std::numeric_limits< int >::max)())))
        BOOST_LOG_THROW_DESCR(bad_alloc, "UTF-16 string too long");

    const boost::winapi::DWORD_ flags =
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
        boost::winapi::WC_ERR_INVALID_CHARS_;
#else
        0u;
#endif
    int len = boost::winapi::WideCharToMultiByte(boost::winapi::CP_UTF8_, flags, wstr, static_cast< int >(utf16_len), NULL, 0, NULL, NULL);
    if (BOOST_LIKELY(len > 0))
    {
        std::string str;
        str.resize(len);

        len = boost::winapi::WideCharToMultiByte(boost::winapi::CP_UTF8_, flags, wstr, static_cast< int >(utf16_len), &str[0], len, NULL, NULL);
        if (BOOST_LIKELY(len > 0))
        {
            return str;
        }
    }

    const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
    BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to convert UTF-16 to UTF-8", (err));
    BOOST_LOG_UNREACHABLE_RETURN(std::string());
}

#endif // defined(BOOST_WINDOWS)

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
