//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/util.hpp>
#include "all_generator.hpp"
#include <locale>

namespace boost { namespace locale { namespace impl_std {
    template<typename CharType>
    std::locale codecvt_bychar(const std::locale& in, const std::string& locale_name)
    {
        return std::locale(in, new std::codecvt_byname<CharType, char, std::mbstate_t>(locale_name));
    }

    std::locale
    create_codecvt(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf)
    {
#if defined(BOOST_WINDOWS)
        // This isn't fully correct:
        // It will treat the 2-Byte wchar_t as UTF-16 encoded while it may be UCS-2
        // std::basic_filebuf explicitely disallows using suche multi-byte codecvts
        // but it works in practice so far, so use it instead of failing for codepoints above U+FFFF
        if(utf != utf8_support::none)
            return util::create_utf8_codecvt(in, type);
#endif
        if(utf == utf8_support::from_wide)
            return util::create_utf8_codecvt(in, type);
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return codecvt_bychar<char>(in, locale_name);
            case char_facet_t::wchar_f: return codecvt_bychar<wchar_t>(in, locale_name);
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#if defined(BOOST_LOCALE_ENABLE_CHAR16_T)
            case char_facet_t::char16_f: return codecvt_bychar<char16_t>(in, locale_name);
#endif
#if defined(BOOST_LOCALE_ENABLE_CHAR32_T)
            case char_facet_t::char32_f: return codecvt_bychar<char32_t>(in, locale_name);
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_std
