/*=============================================================================
    Copyright (c) 2001-2014 Joel de Guzman
    Copyright (c) 2023 Nikita Kniazev

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(BOOST_SPIRIT_X3_UC_TYPES_NOVEMBER_23_2008_0840PM)
#define BOOST_SPIRIT_X3_UC_TYPES_NOVEMBER_23_2008_0840PM

#include <boost/config.hpp>
#include <type_traits>
#include <string>

namespace boost { namespace spirit { namespace x3
{
    typedef char32_t ucs4_char;
    typedef char utf8_char;
    typedef std::basic_string<ucs4_char> ucs4_string;
    typedef std::basic_string<utf8_char> utf8_string;

namespace detail {
    inline void utf8_put_encode(utf8_string& out, ucs4_char x)
    {
        // https://www.unicode.org/versions/Unicode15.0.0/ch03.pdf D90
        if (BOOST_UNLIKELY(x > 0x10FFFFul || (0xD7FFul < x && x < 0xE000ul)))
            x = 0xFFFDul;

        // Table 3-6. UTF-8 Bit Distribution
        if (x < 0x80ul) {
            out.push_back(static_cast<unsigned char>(x));
        }
        else if (x < 0x800ul) {
            out.push_back(static_cast<unsigned char>(0xC0ul + (x >> 6)));
            out.push_back(static_cast<unsigned char>(0x80ul + (x & 0x3Ful)));
        }
        else if (x < 0x10000ul) {
            out.push_back(static_cast<unsigned char>(0xE0ul + (x >> 12)));
            out.push_back(static_cast<unsigned char>(0x80ul + ((x >> 6) & 0x3Ful)));
            out.push_back(static_cast<unsigned char>(0x80ul + (x & 0x3Ful)));
        }
        else {
            out.push_back(static_cast<unsigned char>(0xF0ul + (x >> 18)));
            out.push_back(static_cast<unsigned char>(0x80ul + ((x >> 12) & 0x3Ful)));
            out.push_back(static_cast<unsigned char>(0x80ul + ((x >> 6) & 0x3Ful)));
            out.push_back(static_cast<unsigned char>(0x80ul + (x & 0x3Ful)));
        }
    }
}

    template <typename Char>
    inline utf8_string to_utf8(Char value)
    {
        utf8_string result;
        typedef typename std::make_unsigned<Char>::type UChar;
        detail::utf8_put_encode(result, static_cast<UChar>(value));
        return result;
    }

    template <typename Char>
    inline utf8_string to_utf8(Char const* str)
    {
        utf8_string result;
        typedef typename std::make_unsigned<Char>::type UChar;
        while (*str)
            detail::utf8_put_encode(result, static_cast<UChar>(*str++));
        return result;
    }

    template <typename Char, typename Traits, typename Allocator>
    inline utf8_string
    to_utf8(std::basic_string<Char, Traits, Allocator> const& str)
    {
        utf8_string result;
        typedef typename std::make_unsigned<Char>::type UChar;
        for (Char ch : str)
            detail::utf8_put_encode(result, static_cast<UChar>(ch));
        return result;
    }

    // Assume wchar_t content is UTF-16 on MSVC, or mingw/wineg++ with -fshort-wchar
#if defined(_MSC_VER) || defined(__SIZEOF_WCHAR_T__) && __SIZEOF_WCHAR_T__ == 2
    inline utf8_string to_utf8(wchar_t value)
    {
        utf8_string result;
        detail::utf8_put_encode(result, static_cast<std::make_unsigned<wchar_t>::type>(value));
        return result;
    }

namespace detail {
    inline ucs4_char decode_utf16(wchar_t const*& s)
    {
        typedef std::make_unsigned<wchar_t>::type uwchar_t;

        uwchar_t x(*s);
        if (x < 0xD800ul || x > 0xDFFFul)
            return x;

        // expected high-surrogate
        if (BOOST_UNLIKELY((x >> 10) != 0b110110ul))
            return 0xFFFDul;

        uwchar_t y(*++s);
        // expected low-surrogate
        if (BOOST_UNLIKELY((y >> 10) != 0b110111ul))
            return 0xFFFDul;

        return ((x & 0x3FFul) << 10) + (y & 0x3FFul) + 0x10000ul;
    }
}

    inline utf8_string to_utf8(wchar_t const* str)
    {
        utf8_string result;
        for (ucs4_char c; (c = detail::decode_utf16(str)) != ucs4_char(); ++str)
            detail::utf8_put_encode(result, c);
        return result;
    }

    template <typename Traits, typename Allocator>
    inline utf8_string
    to_utf8(std::basic_string<wchar_t, Traits, Allocator> const& str)
    {
        return to_utf8(str.c_str());
    }
#endif
}}}

#endif
