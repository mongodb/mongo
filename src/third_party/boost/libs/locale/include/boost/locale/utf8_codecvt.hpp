//
// Copyright (c) 2015 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_UTF8_CODECVT_HPP
#define BOOST_LOCALE_UTF8_CODECVT_HPP

#include <boost/locale/generic_codecvt.hpp>
#include <boost/locale/utf.hpp>
#include <boost/assert.hpp>
#include <cstdint>
#include <locale>

namespace boost { namespace locale {

    /// \brief Generic utf8 codecvt facet, it allows to convert UTF-8 strings to UTF-16 and UTF-32 using wchar_t,
    /// char32_t and char16_t
    template<typename CharType>
    class utf8_codecvt : public generic_codecvt<CharType, utf8_codecvt<CharType>> {
    public:
        struct state_type {};

        utf8_codecvt(size_t refs = 0) : generic_codecvt<CharType, utf8_codecvt<CharType>>(refs) {}

        static int max_encoding_length() { return 4; }

        static state_type initial_state(generic_codecvt_base::initial_convertion_state /* unused */)
        {
            return state_type();
        }
        static utf::code_point to_unicode(state_type&, const char*& begin, const char* end)
        {
            const char* p = begin;

            utf::code_point c = utf::utf_traits<char>::decode(p, end);
            if(c != utf::illegal && c != utf::incomplete)
                begin = p;
            return c;
        }

        static utf::len_or_error from_unicode(state_type&, utf::code_point u, char* begin, const char* end)
        {
            BOOST_ASSERT(utf::is_valid_codepoint(u));
            const auto width = utf::utf_traits<char>::width(u);
            if(width > end - begin)
                return utf::incomplete;
            utf::utf_traits<char>::encode(u, begin);
            return width;
        }
    };

}} // namespace boost::locale

#endif
