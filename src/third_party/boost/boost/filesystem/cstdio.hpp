//  boost/filesystem/cstdio.hpp  ------------------------------------------------------//

//  Copyright Andrey Semashev 2023

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_CSTDIO_HPP
#define BOOST_FILESYSTEM_CSTDIO_HPP

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>
#include <cstdio>
#if defined(BOOST_WINDOWS_API)
#include <wchar.h>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#endif

#include <boost/filesystem/detail/header.hpp> // must be the last #include

namespace boost {
namespace filesystem {

#if defined(BOOST_WINDOWS_API)

inline std::FILE* fopen(filesystem::path const& p, const char* mode)
{
#if defined(__CYGWIN__) || (defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR) && defined(__STRICT_ANSI__))
    // Cygwin and MinGW32 in strict ANSI mode do not declare _wfopen
    return std::fopen(p.string().c_str(), mode);
#else
    // mode should only contain ASCII characters and is likely short
    struct small_array
    {
        wchar_t buf[128u];
        wchar_t* p;

        small_array() noexcept : p(buf) {}
        ~small_array() noexcept
        {
            if (BOOST_UNLIKELY(p != buf))
                std::free(p);
        }
    }
    wmode;
    std::size_t wmode_len = std::mbstowcs(wmode.p, mode, sizeof(wmode.buf) / sizeof(wchar_t));
    if (BOOST_UNLIKELY(wmode_len >= (sizeof(wmode.buf) / sizeof(wchar_t))))
    {
        wmode_len = std::mbstowcs(nullptr, mode, 0u);
        // Check for size overflow, including (size_t)-1, which indicates mbstowcs error
        if (BOOST_UNLIKELY(wmode_len >= (static_cast< std::size_t >(-1) / sizeof(wchar_t))))
            return nullptr;

        wmode.p = static_cast< wchar_t* >(std::malloc((wmode_len + 1u) * sizeof(wchar_t)));
        if (BOOST_UNLIKELY(!wmode.p))
            return nullptr;

        std::size_t wmode_len2 = std::mbstowcs(wmode.p, mode, wmode_len + 1u);
        if (BOOST_UNLIKELY(wmode_len2 > wmode_len))
            return nullptr;
    }

    return ::_wfopen(p.c_str(), wmode.p);
#endif
}

#else // defined(BOOST_WINDOWS_API)

inline std::FILE* fopen(filesystem::path const& p, const char* mode)
{
    return std::fopen(p.c_str(), mode);
}

#endif // defined(BOOST_WINDOWS_API)

} // namespace filesystem
} // namespace boost

#include <boost/filesystem/detail/footer.hpp>

#endif // BOOST_FILESYSTEM_CSTDIO_HPP
